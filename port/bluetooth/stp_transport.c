/* Native BTstack transport for /dev/stpbt, without termios or kernel modules. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bluetooth.h"
#include "btstack_run_loop.h"
#include "hci_transport.h"

static btstack_data_source_t source;
static int device = -1;
static uint8_t incoming[4096];
static size_t used;
static void (*packet_handler)(uint8_t, uint8_t *, uint16_t);

static void process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback) {
    (void)ds; (void)callback;
    ssize_t length = read(device, incoming + used, sizeof incoming - used);
    if (length < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (length <= 0) {
        perror("stpbt receive");
        btstack_run_loop_trigger_exit();
        return;
    }
    used += (size_t)length;
    while (used) {
        size_t header, payload;
        switch (incoming[0]) {
        case HCI_EVENT_PACKET:
            header = 3;
            if (used < header) return;
            payload = incoming[2];
            break;
        case HCI_ACL_DATA_PACKET:
            header = 5;
            if (used < header) return;
            payload = incoming[3] | (size_t)incoming[4] << 8;
            break;
        case HCI_SCO_DATA_PACKET:
            header = 4;
            if (used < header) return;
            payload = incoming[3];
            break;
        default:
            fprintf(stderr, "Unexpected H4 type %u\n", incoming[0]);
            btstack_run_loop_trigger_exit();
            return;
        }
        size_t total = header + payload;
        if (total > 2048) {
            fputs("Oversize stpbt H4 packet.\n", stderr);
            btstack_run_loop_trigger_exit();
            return;
        }
        if (used < total) return;
        uint8_t type = incoming[0], packet[2048];
        memcpy(packet, incoming + 1, total - 1);
        used -= total;
        memmove(incoming, incoming + total, used);
        packet_handler(type, packet, (uint16_t)(total - 1));
    }
}

static void init(const void *config) { (void)config; used = 0; }
static int open_transport(void) {
    device = open("/dev/stpbt", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device < 0) { perror("open stpbt"); return -1; }
    btstack_run_loop_set_data_source_fd(&source, device);
    btstack_run_loop_set_data_source_handler(&source, process);
    btstack_run_loop_enable_data_source_callbacks(&source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&source);
    return 0;
}
static int close_transport(void) {
    if (device >= 0) {
        btstack_run_loop_remove_data_source(&source);
        close(device);
        device = -1;
    }
    used = 0;
    return 0;
}
static void register_handler(void (*handler)(uint8_t, uint8_t *, uint16_t)) { packet_handler = handler; }
static int send_packet(uint8_t type, uint8_t *packet, int size) {
    uint8_t frame[2048];
    if (size < 0 || size + 1 > (int)sizeof frame) return -1;
    frame[0] = type;
    memcpy(frame + 1, packet, (size_t)size);
    for (int attempt = 0; attempt < 10; attempt++) {
        ssize_t sent = write(device, frame, (size_t)size + 1);
        if (sent == size + 1) return 0;
        if (sent >= 0 || (errno != EAGAIN && errno != EINTR)) break;
        struct timespec delay = { .tv_nsec = 20000000 };
        nanosleep(&delay, NULL);
    }
    perror("stpbt send");
    return -1;
}
static const hci_transport_t transport = {
    .name = "X620 STP native", .init = init, .open = open_transport, .close = close_transport,
    .register_packet_handler = register_handler, .send_packet = send_packet,
    /* NULL can_send_packet_now marks this bounded, synchronous transport. */
};
const hci_transport_t *native_stp_transport(void) { return &transport; }
