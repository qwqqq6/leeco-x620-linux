/* Manual panel power control for the X620, independent of system suspend.
 * Only root/desktop may request the fixed operations on the local socket.
 * A short hardware power-key press toggles the panel; volume keys are untouched.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_PATH "/run/native-desktop/screen.sock"
#define BACKLIGHT "/sys/class/leds/lcd-backlight/brightness"
#define BLANK "/sys/class/graphics/fb0/blank"

static volatile sig_atomic_t stopping;
static int power_fd = -1, touch_fd = -1, saved_brightness = 120;
static pid_t paused_refresh;
static bool screen_off, touch_grabbed;

static void on_signal(int number) { (void)number; stopping = 1; }

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int write_attribute(const char *path, int value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    char text[24];
    int length = snprintf(text, sizeof(text), "%d\n", value);
    ssize_t result;
    do { result = write(fd, text, (size_t)length); } while (result < 0 && errno == EINTR);
    int error = errno;
    close(fd);
    if (result != length) { errno = result < 0 ? error : EIO; return -1; }
    return 0;
}

static int read_brightness(void)
{
    FILE *stream = fopen(BACKLIGHT, "r");
    int value = 0;
    if (stream) { if (fscanf(stream, "%d", &value) != 1) value = 0; fclose(stream); }
    return value;
}

static int input_by_name(const char *expected)
{
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    if (glob("/sys/class/input/event*/device/name", 0, NULL, &matches)) return -1;
    int result = -1;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        char name[256], event[48], path[80];
        FILE *stream = fopen(matches.gl_pathv[i], "r");
        if (!stream) continue;
        bool match = fgets(name, sizeof(name), stream) &&
                     !strncmp(name, expected, strlen(expected)) &&
                     (name[strlen(expected)] == '\n' || name[strlen(expected)] == '\0');
        fclose(stream);
        if (!match || sscanf(matches.gl_pathv[i], "/sys/class/input/%47[^/]/device/name", event) != 1)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", event);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, expected)) {
            result = fd;
            break;
        }
        close(fd);
    }
    globfree(&matches);
    return result;
}

static pid_t refresh_pid(void)
{
    FILE *stream = fopen("/run/native-desktop/refresh.pid", "r");
    long value = 0;
    if (!stream) return 0;
    if (fscanf(stream, "%ld", &value) != 1) value = 0;
    fclose(stream);
    if (value <= 1 || value > 4194304) return 0;
    char path[80], executable[256];
    snprintf(path, sizeof(path), "/proc/%ld/exe", value);
    ssize_t length = readlink(path, executable, sizeof(executable) - 1);
    if (length < 0) return 0;
    executable[length] = '\0';
    return !strcmp(executable, "/usr/local/libexec/native-display-refresh") ? (pid_t)value : 0;
}

static int pause_refresh(void)
{
    paused_refresh = refresh_pid();
    if (!paused_refresh || kill(paused_refresh, SIGSTOP)) return -1;
    char path[80], stat[512];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)paused_refresh);
    for (int attempt = 0; attempt < 100; attempt++) {
        FILE *stream = fopen(path, "r");
        if (!stream) break;
        bool read = fgets(stat, sizeof(stat), stream) != NULL;
        fclose(stream);
        char *end = read ? strrchr(stat, ')') : NULL;
        if (end && end[1] == ' ' && end[2] == 'T') return 0;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&delay, NULL);
    }
    kill(paused_refresh, SIGCONT);
    paused_refresh = 0;
    errno = ETIMEDOUT;
    return -1;
}

static void drain_touch(void)
{
    struct input_event events[64];
    while (read(touch_fd, events, sizeof(events)) > 0) {}
}

static int turn_on(void)
{
    int failed = 0;
    if (write_attribute(BLANK, 0)) failed = 1;
    if (write_attribute(BACKLIGHT, saved_brightness)) failed = 1;
    if (paused_refresh && refresh_pid() == paused_refresh) {
        if (kill(paused_refresh, SIGCONT)) failed = 1;
    }
    paused_refresh = 0;
    if (touch_grabbed) {
        drain_touch();
        if (ioctl(touch_fd, EVIOCGRAB, 0)) failed = 1;
        touch_grabbed = false;
    }
    screen_off = false;
    puts(failed ? "SCREEN_WAKE_FAILED" : "SCREEN_ON");
    return failed ? -1 : 0;
}

static int turn_off(void)
{
    if (screen_off) return 0;
    int brightness = read_brightness();
    if (brightness > 0 && brightness <= 255) saved_brightness = brightness;
    if (pause_refresh()) return -1;
    if (ioctl(touch_fd, EVIOCGRAB, 1)) {
        turn_on();
        return -1;
    }
    touch_grabbed = true;
    if (write_attribute(BACKLIGHT, 0) || write_attribute(BLANK, 4)) {
        turn_on();
        return -1;
    }
    screen_off = true;
    puts("SCREEN_OFF refresh=paused touch=guarded system_suspend=no");
    return 0;
}

static int make_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, SOCKET_PATH);
    /* service.lock has already excluded another controller instance. */
    if (unlink(SOCKET_PATH) && errno != ENOENT) { close(fd); return -1; }
    mode_t old = umask(0077);
    int result = bind(fd, (struct sockaddr *)&address, sizeof(address));
    umask(old);
    if (result || chown(SOCKET_PATH, 1000, 0) || chmod(SOCKET_PATH, 0600) || listen(fd, 4)) {
        close(fd);
        unlink(SOCKET_PATH);
        return -1;
    }
    return fd;
}

static void handle_request(int listener)
{
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0) return;
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
        (peer.uid != 0 && peer.uid != 1000)) {
        close(client);
        return;
    }
    struct pollfd pending = { .fd = client, .events = POLLIN };
    char request[24] = {0}, answer[160];
    if (poll(&pending, 1, 200) <= 0) { close(client); return; }
    ssize_t length = recv(client, request, sizeof(request), MSG_TRUNC);
    int result = 0;
    if (length <= 0 || length >= (ssize_t)sizeof(request)) result = -1;
    else if (!strcmp(request, "off")) result = turn_off();
    else if (!strcmp(request, "on")) { if (screen_off) result = turn_on(); }
    else if (!strcmp(request, "toggle")) result = screen_off ? turn_on() : turn_off();
    else if (strcmp(request, "status")) result = -1;
    snprintf(answer, sizeof(answer), "%s state=%s brightness=%d refresh=%s touch_guard=%s\n",
             result ? "ERROR" : "SCREEN", screen_off ? "off" : "on", read_brightness(),
             screen_off ? "paused" : "running", touch_grabbed ? "on" : "off");
    send(client, answer, strlen(answer), MSG_NOSIGNAL);
    close(client);
}

static int control(const char *operation)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("screen socket"); return 1; }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        send(fd, operation, strlen(operation), MSG_NOSIGNAL) != (ssize_t)strlen(operation)) {
        perror("screen control"); close(fd); return 1;
    }
    struct pollfd pending = { .fd = fd, .events = POLLIN };
    if (poll(&pending, 1, 2500) <= 0) {
        fputs("Screen controller did not respond.\n", stderr); close(fd); return 1;
    }
    char answer[256];
    ssize_t length = recv(fd, answer, sizeof(answer) - 1, 0);
    close(fd);
    if (length <= 0) return 1;
    answer[length] = '\0';
    fputs(answer, stdout);
    return strncmp(answer, "SCREEN ", 7) != 0;
}

static int daemon_main(void)
{
    if (geteuid() != 0) { fputs("Screen daemon requires root.\n", stderr); return 1; }
    int lock = open("/run/native-desktop/screen.lock", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        fputs("Screen controller already running or lock unavailable.\n", stderr);
        if (lock >= 0) close(lock);
        return 1;
    }
    power_fd = input_by_name("mtk-kpd");
    touch_fd = input_by_name("synaptics_dsx_i2c");
    if (power_fd < 0 || touch_fd < 0) {
        fputs("Screen control input devices unavailable.\n", stderr);
        if (power_fd >= 0) close(power_fd);
        if (touch_fd >= 0) close(touch_fd);
        close(lock);
        return 1;
    }
    struct sigaction action = { .sa_handler = on_signal };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    int brightness = read_brightness();
    if (brightness > 0 && brightness <= 255) saved_brightness = brightness;
    int listener = make_socket();
    if (listener < 0) { perror("screen listener"); close(power_fd); close(touch_fd); close(lock); return 1; }
    long long pressed_at = -1, last_toggle = 0;
    struct pollfd fds[] = {
        { .fd = listener, .events = POLLIN },
        { .fd = power_fd, .events = POLLIN },
        { .fd = touch_fd, .events = POLLIN },
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("SCREEN_CONTROL_READY short_power_press=toggle auto_off=disabled");
    while (!stopping) {
        int result = poll(fds, 3, 1000);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & POLLIN) handle_request(listener);
        if (fds[1].revents & POLLIN) {
            struct input_event events[32];
            ssize_t length;
            while ((length = read(power_fd, events, sizeof(events))) > 0) {
                for (size_t i = 0; i < (size_t)length / sizeof(events[0]); i++) {
                    if (events[i].type != EV_KEY || events[i].code != KEY_POWER) continue;
                    long long now = monotonic_ms();
                    if (events[i].value == 1) pressed_at = now;
                    else if (events[i].value == 0 && pressed_at >= 0) {
                        if (now - pressed_at < 1500 && now - last_toggle > 400) {
                            if (screen_off ? turn_on() : turn_off()) perror("power key panel control");
                            last_toggle = now;
                        }
                        pressed_at = -1;
                    }
                }
            }
        }
        if (fds[2].revents & POLLIN) drain_touch();
        if ((fds[1].revents | fds[2].revents) & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    if (screen_off || paused_refresh || touch_grabbed) turn_on();
    close(listener);
    unlink(SOCKET_PATH);
    close(power_fd);
    close(touch_fd);
    close(lock);
    return stopping ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--daemon")) return daemon_main();
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off") ||
                      !strcmp(argv[1], "toggle") || !strcmp(argv[1], "status"))) return control(argv[1]);
    fputs("native-screen on|off|toggle|status\n", argc == 2 && !strcmp(argv[1], "--help") ? stdout : stderr);
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}
