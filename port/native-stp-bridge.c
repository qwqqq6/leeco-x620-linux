/* Bounded diagnostic H4 transport for the vendor stpbt driver.
 * Read must be nonblocking: the old driver's blocking wait is uninterruptible.
 * Only diagnostic command opcodes are accepted. No connections or pairing.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number) { stopped = signal_number; }

static int allowed(unsigned opcode) {
    switch (opcode) {
    case 0x0c03: /* Reset */
    case 0x1001: /* Local version */
    case 0x1003: /* Local features */
    case 0x1009: /* Local public address */
    case 0x2003: /* LE local features */
    case 0x0c01: /* Event mask */
    case 0x2001: /* LE event mask */
    case 0x200b: /* LE scan parameters */
    case 0x200c: /* LE scan enable */
    case 0x0c45: /* Inquiry mode */
    case 0x0401: /* Inquiry */
    case 0x0402: /* Inquiry cancel */
    case 0x0419: /* Remote name request */
    case 0x041a: /* Remote name request cancel */
        return 1;
    default:
        return 0;
    }
}

static int write_stdout(const uint8_t *data, size_t length) {
    while (length && !stopped) {
        ssize_t sent = write(STDOUT_FILENO, data, length);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        data += sent;
        length -= (size_t)sent;
    }
    return length ? -1 : 0;
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1 || geteuid() != 0) {
        fputs("Root diagnostic transport; takes no arguments.\n", stderr);
        return 64;
    }
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGALRM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    alarm(90);
    int lock = open("/run/native-bluetooth.lock", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        fputs("Bluetooth diagnostic transport is already in use.\n", stderr);
        if (lock >= 0) close(lock);
        return 73;
    }
    int device = open("/dev/stpbt", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device < 0) { perror("open stpbt"); close(lock); return 1; }
    uint8_t input[4096], output[1024];
    size_t used = 0;
    int result = 0;
    while (!stopped) {
        int retry = 0;
        while (used >= 4) {
            unsigned opcode = input[1] | (unsigned)input[2] << 8;
            if (input[0] != 1 || !allowed(opcode)) {
                fputs("Rejected non-diagnostic HCI packet.\n", stderr);
                result = 65;
                goto done;
            }
            size_t length = 4 + (size_t)input[3];
            if (used < length) break;
            ssize_t sent = write(device, input, length);
            if (sent < 0 && (errno == EAGAIN || errno == EINTR)) { retry = 1; break; }
            if (sent != (ssize_t)length) {
                perror("write complete HCI packet");
                result = 1;
                goto done;
            }
            used -= length;
            memmove(input, input + length, used);
        }
        struct pollfd descriptors[2] = {
            { .fd = STDIN_FILENO, .events = used < sizeof input ? POLLIN : 0 },
            { .fd = device, .events = POLLIN },
        };
        int ready = poll(descriptors, 2, retry ? 20 : 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); result = 1; break; }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            ssize_t got = read(STDIN_FILENO, input + used, sizeof input - used);
            if (got == 0) { if (used) result = 65; break; }
            if (got < 0 && errno != EINTR) { perror("stdin"); result = 1; break; }
            if (got > 0) used += (size_t)got;
        }
        if (descriptors[1].revents & POLLIN) {
            ssize_t got = read(device, output, sizeof output);
            if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (got <= 0 || write_stdout(output, (size_t)got) < 0) {
                perror("stpbt read or stdout"); result = 1; break;
            }
        }
        if ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLNVAL)) {
            fputs("Transport poll error.\n", stderr); result = 1; break;
        }
    }
done:
    close(device); /* Vendor release powers off Bluetooth; Wi-Fi stays independent. */
    close(lock);
    alarm(0);
    return stopped == SIGALRM ? 124 : (stopped ? 128 + stopped : result);
}
