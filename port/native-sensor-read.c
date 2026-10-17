/* On-demand access to the original MTK sensor input interfaces.
 * Root diagnostic tool; no daemon, calibration, or firmware writes.
 * Event layout follows Linux UAPI input_event on native AArch64.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct event { struct timeval time; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct event) == 24, "Expected native 64-bit input_event");
struct sensor { const char *name, *input, *base; };
static const struct sensor sensors[] = {
    {"accel", "m_acc_input", "/sys/class/misc/m_acc_misc/acc"},
    {"gyro", "m_gyro_input", "/sys/class/misc/m_gyro_misc/gyro"},
    {"mag", "m_mag_input", "/sys/class/misc/m_mag_misc/mag"},
    {"light", "m_alsps_input", "/sys/class/misc/m_alsps_misc/als"},
    {"proximity", "m_alsps_input", "/sys/class/misc/m_alsps_misc/ps"},
};
static volatile sig_atomic_t stop;
static char active_path[160];
static int enabled;

static int write_setting(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t length = strlen(value);
    ssize_t done = write(fd, value, length);
    int saved = errno;
    if (done >= 0 && (size_t)done != length) saved = EIO;
    close(fd);
    errno = saved;
    return done == (ssize_t)length ? 0 : -1;
}
static void cleanup(void) {
    if (enabled && write_setting(active_path, "0\n") < 0) perror("disable sensor");
}
static void on_signal(int signum) { (void)signum; stop = 1; }
static double monotonic(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (double)now.tv_sec + now.tv_nsec / 1000000000.0;
}
static int open_input(const char *wanted) {
    glob_t paths = {0};
    int fd = -1;
    if (glob("/sys/class/input/event*/device/name", 0, NULL, &paths) != 0) return -1;
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        FILE *file = fopen(paths.gl_pathv[i], "r");
        if (!file) continue;
        char name[96];
        int valid = fgets(name, sizeof name, file) != NULL;
        fclose(file);
        if (!valid) continue;
        name[strcspn(name, "\r\n")] = 0;
        if (strcmp(name, wanted)) continue;
        char node[96];
        const char *event = paths.gl_pathv[i] + strlen("/sys/class/input/");
        size_t length = strcspn(event, "/");
        if (length > 30) continue;
        snprintf(node, sizeof node, "/dev/input/%.*s", (int)length, event);
        fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        break;
    }
    globfree(&paths);
    return fd;
}
int main(int argc, char **argv) {
    const struct sensor *sensor = NULL;
    if (argc == 3) {
        for (size_t i = 0; i < sizeof sensors / sizeof sensors[0]; i++)
            if (!strcmp(argv[1], sensors[i].name)) sensor = &sensors[i];
    }
    char *end = NULL;
    long seconds = argc == 3 ? strtol(argv[2], &end, 10) : 0;
    if (!sensor || !end || *end || seconds < 1 || seconds > 60) {
        fprintf(stderr, "Usage: native-sensor-read accel|gyro|mag|light|proximity SECONDS (1-60)\n");
        return 2;
    }
    if (geteuid() != 0) { fprintf(stderr, "Run through the root SSH session.\n"); return 1; }
    char lock_path[96];
    snprintf(lock_path, sizeof lock_path, "/run/native-sensor-read-%s.lock", sensor->name);
    int lock = open(lock_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "Sensor is already in use, or its lock is unavailable.\n");
        return 1;
    }
    int fd = open_input(sensor->input);
    if (fd < 0) { perror("open sensor input"); return 1; }
    snprintf(active_path, sizeof active_path, "%sactive", sensor->base);
    char delay_path[160];
    snprintf(delay_path, sizeof delay_path, "%sdelay", sensor->base);
    if (write_setting(delay_path, "200000000\n") < 0) { perror("set 200 ms delay"); close(fd); return 1; }
    atexit(cleanup);
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGPIPE, &action, NULL);
    if (write_setting(active_path, "1\n") < 0) { perror("enable sensor"); close(fd); return 1; }
    enabled = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "sensor=%s input=%s period_ns=200000000 seconds=%ld\n",
            sensor->name, sensor->input, seconds);
    double deadline = monotonic() + seconds;
    struct pollfd polling = {.fd = fd, .events = POLLIN};
    int failed = 0;
    unsigned long count = 0;
    while (!stop && monotonic() < deadline) {
        int ready = poll(&polling, 1, 200);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll input"); failed = 1; break; }
        if (!ready) continue;
        if (polling.revents & (POLLERR | POLLHUP | POLLNVAL)) { failed = 1; break; }
        struct event events[32];
        ssize_t bytes = read(fd, events, sizeof events);
        if (bytes < 0) { if (errno == EAGAIN || errno == EINTR) continue; perror("read input"); failed = 1; break; }
        if (bytes == 0 || bytes % (ssize_t)sizeof(struct event)) { failed = 1; break; }
        for (size_t i = 0; i < (size_t)bytes / sizeof(struct event); i++) {
            struct event *e = &events[i];
            if (e->type != 2 && e->type != 3) continue;
            printf("{\"sensor\":\"%s\",\"time\":%lld.%06ld,\"type\":%u,\"code\":%u,\"value\":%d}\n",
                   sensor->name, (long long)e->time.tv_sec, (long)e->time.tv_usec,
                   e->type, e->code, e->value);
            count++;
        }
    }
    close(fd);
    fprintf(stderr, "events=%lu; sensor disabled on exit\n", count);
    return failed ? 1 : 0;
}
