/* Pace the existing MTK fb0 pan submission without spawning sleep processes. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t report_requested;

static void on_signal(int number)
{
    if (number == SIGUSR1) report_requested = 1;
    else stopping = 1;
}

static int64_t clock_ns(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (int64_t)value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static int parse_number(const char *text, int minimum, int maximum)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || text == end || *end || value < minimum || value > maximum)
        return -1;
    return (int)value;
}

static void report(uint64_t frames, int64_t start, int64_t cpu_start,
                   int64_t pan_total, int64_t pan_max, uint64_t skipped)
{
    double seconds = (clock_ns(CLOCK_MONOTONIC) - start) / 1e9;
    printf("DISPLAY_REFRESH frames=%" PRIu64 " elapsed=%.3f rate=%.3f"
           " pan_mean_ms=%.3f pan_max_ms=%.3f cpu_ms=%.3f"
           " skipped_periods=%" PRIu64 "\n",
           frames, seconds, seconds > 0 ? frames / seconds : 0,
           frames ? pan_total / (double)frames / 1e6 : 0, pan_max / 1e6,
           (clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu_start) / 1e6, skipped);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int fps = 60, duration = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") && argc == 2) {
            puts("native-display-refresh [--fps 1..60] [--seconds 1..60]");
            return 0;
        }
        if (i + 1 >= argc) goto usage;
        if (!strcmp(argv[i], "--fps")) {
            fps = parse_number(argv[++i], 1, 60);
            if (fps < 0) goto usage;
        } else if (!strcmp(argv[i], "--seconds")) {
            duration = parse_number(argv[++i], 1, 60);
            if (duration < 0) goto usage;
        } else goto usage;
    }
    if (geteuid() != 0) {
        fputs("Display refresh requires root.\n", stderr);
        return 1;
    }
    int lock = open("/run/native-desktop/refresh.lock",
                    O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) { perror("refresh lock"); return 1; }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fputs("Display refresher already running.\n", stderr);
        close(lock);
        return 2;
    }
    int pan = open("/sys/class/graphics/fb0/pan", O_WRONLY | O_CLOEXEC);
    if (pan < 0) { perror("open fb0/pan"); close(lock); return 1; }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL) || sigaction(SIGUSR1, &action, NULL)) {
        perror("sigaction"); close(pan); close(lock); return 1;
    }
    int64_t start = clock_ns(CLOCK_MONOTONIC);
    int64_t cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    int64_t period = INT64_C(1000000000) / fps, deadline = start;
    int64_t end = duration ? start + (int64_t)duration * INT64_C(1000000000) : 0;
    int64_t pan_total = 0, pan_max = 0;
    uint64_t frames = 0, skipped = 0;
    int failed = 0, initial_report = 0;
    printf("DISPLAY_REFRESH_STARTED target_hz=%d seconds=%d method=fb0_pan\n",
           fps, duration);
    fflush(stdout);
    while (!stopping && (!end || clock_ns(CLOCK_MONOTONIC) < end)) {
        int64_t before = clock_ns(CLOCK_MONOTONIC);
        if (lseek(pan, 0, SEEK_SET) < 0) {
            perror("seek fb0/pan"); failed = 1; break;
        }
        ssize_t result;
        do { result = write(pan, "0,0\n", 4); }
        while (result < 0 && errno == EINTR && !stopping);
        if (result != 4) {
            if (stopping && result < 0 && errno == EINTR) break;
            if (result < 0) perror("write fb0/pan");
            else fputs("Short fb0/pan write.\n", stderr);
            failed = 1; break;
        }
        int64_t after = clock_ns(CLOCK_MONOTONIC), elapsed = after - before;
        frames++;
        pan_total += elapsed;
        if (elapsed > pan_max) pan_max = elapsed;
        if ((!initial_report && frames >= (uint64_t)fps) || report_requested) {
            report(frames, start, cpu_start, pan_total, pan_max, skipped);
            initial_report = 1;
            report_requested = 0;
        }
        deadline += period;
        /* A blocking pan often consumes almost a complete display period.
         * Do not add another period when it finishes a little past deadline.
         * Skip only entire stale periods, and never burst to catch up. */
        if (after - deadline >= period) {
            uint64_t late = (uint64_t)((after - deadline) / period);
            skipped += late;
            deadline += (int64_t)late * period;
        }
        if (deadline > after && !stopping) {
            struct timespec until = { .tv_sec = (time_t)(deadline / INT64_C(1000000000)),
                                      .tv_nsec = (long)(deadline % INT64_C(1000000000)) };
            int error;
            do { error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL); }
            while (error == EINTR && !stopping);
            if (error && error != EINTR) {
                errno = error; perror("clock_nanosleep"); failed = 1; break;
            }
        }
    }
    report(frames, start, cpu_start, pan_total, pan_max, skipped);
    close(pan);
    close(lock);
    return failed;
usage:
    fputs("Usage: native-display-refresh [--fps 1..60] [--seconds 1..60]\n", stderr);
    return 2;
}
