/* Native musl client for the original, audited X620 HAL/MNL datagram wire.
 * Stage 0 listens only. Stage 1 initializes then stops, no start command.
 * Stage 2 starts a bounded standalone session and reports NMEA statistics.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static volatile sig_atomic_t stopped;
static int fd;
static unsigned packets, nmea_count, valid_checksums, bad_checksums, satellites_visible, satellites_used, best_snr;
static int fix_valid, fix_dimension, engine_off, wake_held;
static unsigned time_injections;
static int command(unsigned opcode, const uint32_t *args, unsigned argc);
static void stop(int signo) { stopped = signo; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { for (unsigned n=0; n<4; n++) p[n] = (uint8_t)(v >> (8*n)); }
static int bounded_number(const char *s, int low, int high) {
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    return !errno && *s && !*end && value >= low && value <= high ? (int)value : -1;
}
static void wake_lock(int acquire) {
    if (acquire == wake_held) return;
    int file = open(acquire ? "/sys/power/wake_lock" : "/sys/power/wake_unlock", O_WRONLY|O_CLOEXEC);
    if (file < 0) { perror("GNSS wake lock"); return; }
    const char tag[] = "native-gnss";
    if (write(file, tag, sizeof tag-1) == sizeof tag-1) {
        wake_held = acquire;
        printf("GNSS_WAKE_LOCK held=%d\n", wake_held);
    } else perror("GNSS wake lock write");
    close(file);
}
static void inject_time(void) {
    struct timespec real_time, boot_time;
    if (clock_gettime(CLOCK_REALTIME, &real_time) || clock_gettime(CLOCK_BOOTTIME, &boot_time) ||
        real_time.tv_sec < 1577836800) return;
    uint64_t unix_ms = (uint64_t)real_time.tv_sec*1000 + real_time.tv_nsec/1000000;
    uint64_t boot_ms = (uint64_t)boot_time.tv_sec*1000 + boot_time.tv_nsec/1000000;
    /* Original HAL: put_int(1), put_int(105), put_long(UTC milliseconds),
     * put_long(elapsedRealtime milliseconds), put_int(uncertainty). */
    const uint32_t timing[] = {unix_ms, unix_ms >> 32, boot_ms, boot_ms >> 32, 1000};
    if (!command(105, timing, 5)) time_injections++;
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    return -1;
}
static unsigned small_number(const char *s) {
    char *end;
    unsigned long value = strtoul(s, &end, 10);
    return *s && !*end && value <= 99 ? (unsigned)value : 0;
}
static void inspect_nmea(const uint8_t *data, unsigned length) {
    if (length < 7 || length >= 512 || data[0] != '$') return;
    char sentence[512]; memcpy(sentence, data, length); sentence[length] = 0;
    char *star = strchr(sentence, '*');
    if (!star || !star[1] || !star[2]) return;
    int high = hex_digit(star[1]), low = hex_digit(star[2]);
    if (high < 0 || low < 0) return;
    unsigned checksum = 0;
    for (const char *p=sentence+1; p<star; p++) checksum ^= (unsigned char)*p;
    if (checksum != (unsigned)(high*16+low)) { bad_checksums++; return; }
    valid_checksums++; *star = 0;
    char *fields[32] = {sentence}; unsigned count = 1;
    for (char *p=sentence; *p && count<32; p++) if (*p==',') { *p=0; fields[count++]=p+1; }
    if (strlen(fields[0]) != 6) return;
    const char *type = fields[0]+3;
    if (!strcmp(type,"GGA") && count>7) {
        unsigned used = small_number(fields[7]);
        if (used > satellites_used) satellites_used = used;
        if (small_number(fields[6])) fix_valid = 1;
    } else if (!strcmp(type,"GSA") && count>2) {
        int dimension = (int)small_number(fields[2]);
        if (dimension > fix_dimension && dimension <= 3) fix_dimension = dimension;
    } else if (!strcmp(type,"GSV") && count>3) {
        unsigned visible = small_number(fields[3]);
        if (visible > satellites_visible) satellites_visible = visible;
        for (unsigned i=7; i<count; i+=4) {
            unsigned snr = small_number(fields[i]);
            if (snr > best_snr) best_snr = snr;
        }
    } else if (!strcmp(type,"RMC") && count>2 && !strcmp(fields[2],"A")) fix_valid = 1;
}
static void receive_event(void) {
    uint8_t buffer[16384];
    ssize_t n = recv(fd, buffer, sizeof buffer, 0);
    if (n < 8 || get32(buffer) != 1) return;
    unsigned opcode = get32(buffer+4); packets++;
    if (opcode == 104 && n >= 21) {
        unsigned length = get32(buffer+17);
        if (buffer[16] && length > 0 && length <= (unsigned)n-21) {
            nmea_count++;
            inspect_nmea(buffer+21, length);
            if (nmea_count <= 5) printf("NMEA_RECEIVED bytes=%u\n", length);
        }
    } else if (opcode == 102 && n == 16 && get32(buffer+8) == 4) {
        unsigned status = get32(buffer+12);
        printf("GPS_STATUS status=%u\n", status);
        if (status == 4) engine_off = 1;
    } else if (opcode == 201 && n == 8) wake_lock(1);
    else if (opcode == 202 && n == 8) wake_lock(0);
    else if (opcode == 301 && n == 8) inject_time();
    else printf("EVENT opcode=%u bytes=%zd\n", opcode, n);
}
static int command(unsigned opcode, const uint32_t *args, unsigned argc) {
    struct sockaddr_un destination = { .sun_family = AF_UNIX };
    memcpy(destination.sun_path + 1, "mtk_hal2mnl", 11);
    uint8_t buffer[64] = {0};
    put32(buffer, 1); put32(buffer+4, opcode);
    for (unsigned i=0; i<argc; i++) put32(buffer+8+4*i, args[i]);
    ssize_t sent = sendto(fd, buffer, 8+4*argc, 0, (struct sockaddr *)&destination, sizeof destination);
    printf("COMMAND opcode=%u result=%zd\n", opcode, sent);
    return sent == (ssize_t)(8+4*argc) ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc != 4 || geteuid() != 0) return 64;
    if (strcmp(argv[3], "/tmp/native-gnss-probe") && strcmp(argv[3], "/run/native-gnss/root")) return 64;
    int stage = bounded_number(argv[1], 0, 2), seconds = bounded_number(argv[2], 3, 300);
    if (stage < 0 || seconds < 0) return 64;
    setvbuf(stdout, NULL, _IOLBF, 0);
    int lock = open("/run/native-gnss.lock", O_WRONLY|O_CREAT|O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX|LOCK_NB)) return 73;
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGHUP, stop);
    fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("GNSS client socket"); return 1; }
    struct sockaddr_un local = { .sun_family = AF_UNIX };
    memcpy(local.sun_path + 1, "mtk_mnl2hal", 11);
    /* The original uses a full 110-byte address, including trailing zeros. */
    if (bind(fd, (struct sockaddr *)&local, sizeof local)) { perror("GNSS bind"); return 1; }
    puts("GNSS_CLIENT_READY");
    char log_path[100]; snprintf(log_path, sizeof log_path, "%s/mnld.log", argv[3]);
    int log_fd = open(log_path, O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
    if (log_fd < 0) { perror("GNSS log"); return 1; }
    pid_t child = fork();
    if (child < 0) { perror("GNSS fork"); return 1; }
    if (child == 0) {
        if (dup2(log_fd, 1) < 0 || dup2(log_fd, 2) < 0 ||
            chroot(argv[3]) || chdir("/")) _exit(126);
        setenv("ANDROID_ROOT", "/system", 1);
        setenv("ANDROID_DATA", "/data", 1);
        setenv("LD_PRELOAD", "/system/lib/libnativegnssprops.so", 1);
        setenv("LD_LIBRARY_PATH", "/system/lib", 1);
        execl("/system/xbin/mnld", "mnld", (char *)NULL);
        _exit(127);
    }
    close(log_fd);
    double start = now(), started_at = 0;
    int initialized = 0, started = 0, child_status = 0, reaped = 0;
    while (!stopped && now()-start < seconds) {
        if (waitpid(child, &child_status, WNOHANG) == child) { reaped = 1; break; }
        if (stage && !initialized && now()-start > 2) {
            if (command(101, NULL, 0) == 0) initialized = 1;
        }
        if (stage == 2 && initialized && !started && now()-start > 3) {
            const uint32_t mode[] = {0, 0, 1000, 0, 0};
            if (!command(108, mode, 5) && !command(102, NULL, 0)) {
                started = 1; started_at = now();
            }
        }
        if (started && !time_injections && now()-started_at > 1) inject_time();
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int rc = poll(&p, 1, 200);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) continue;
        receive_event();
    }
    if (started && !reaped) command(103, NULL, 0);
    double stop_at = now();
    while (started && !reaped && !engine_off && now()-stop_at < 3) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 100) > 0) receive_event();
    }
    if (initialized && !reaped) command(104, NULL, 0);
    struct timespec delay = { .tv_nsec = 200000000 };
    if (initialized) { nanosleep(&delay, NULL); nanosleep(&delay, NULL); }
    close(fd);
    if (!reaped) kill(child, SIGTERM);
    for (unsigned attempt=0; !reaped && attempt<15; attempt++) {
        if (waitpid(child, &child_status, WNOHANG) == child) { reaped = 1; break; }
        nanosleep(&delay, NULL);
    }
    if (!reaped) {
        kill(child, SIGKILL);
        pid_t result;
        do { result = waitpid(child, &child_status, 0); } while (result < 0 && errno == EINTR);
        if (result != child) { perror("GNSS wait"); wake_lock(0); return 1; }
    }
    wake_lock(0);
    close(lock);
    printf("MNLD_CHILD_REAPED status=%d forced=%d\n", child_status, !reaped);
    printf("GNSS_SUMMARY packets=%u nmea=%u checksum_ok=%u checksum_bad=%u satellites_visible=%u satellites_used=%u best_snr=%u fix_valid=%d fix_dimension=%d initialized=%d started=%d engine_off=%d time_injections=%u wake_held=%d interrupted=%d\n",
           packets, nmea_count, valid_checksums, bad_checksums, satellites_visible, satellites_used, best_snr,
           fix_valid, fix_dimension, initialized, started, engine_off, time_injections, wake_held, stopped);
    if (stopped) return 128+stopped;
    return packets && (stage != 2 || (started && valid_checksums && engine_off)) && !wake_held ? 0 : 1;
}
