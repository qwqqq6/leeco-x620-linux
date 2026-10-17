/* Reproduce the audited original X620 BT preload parameters, in controller RAM.
 * libbluetooth_mtk.so Thumb addresses: 2310 (address), 1e48 (radio),
 * 1da0 (TX offsets), 1cf0 (sleep). The original config has a 4-byte chip-id
 * prefix before its copied 64-byte NVRAM record. Never open NVRAM writable.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "btstack_chipset.h"

static uint8_t factory[66];
static unsigned command_index;
static unsigned completed;
static int failed;

int native_bt_factory_load(void) {
    int fd = open("/nvdata/APCFG/APRDEB/BT_Addr", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("read factory Bluetooth parameters"); return -1; }
    ssize_t length = read(fd, factory, sizeof factory);
    close(fd);
    const uint8_t zero[6] = {0}, ones[6] = {255,255,255,255,255,255};
    if ((length != 64 && length != 66) || !memcmp(factory, zero, 6) ||
        !memcmp(factory, ones, 6) || (factory[0] & 1)) {
        fputs("Invalid original Bluetooth address or parameter record.\n", stderr);
        return -1;
    }
    return 0;
}
static void init(const void *config) {
    (void)config;
    command_index = completed = 0;
    failed = 0;
}
static btstack_chipset_result_t next_command(uint8_t *command) {
    static const struct { uint16_t opcode; unsigned offset, length; } commands[] = {
        {0xfc1a, 0, 6}, {0xfc79, 12, 6}, {0xfc93, 27, 3}, {0xfc7a, 18, 7},
    };
    if (command_index >= sizeof commands / sizeof commands[0]) return BTSTACK_CHIPSET_DONE;
    unsigned index = command_index++;
    command[0] = (uint8_t)commands[index].opcode;
    command[1] = (uint8_t)(commands[index].opcode >> 8);
    command[2] = (uint8_t)commands[index].length;
    if (index == 0) {
        for (unsigned i = 0; i < 6; i++) command[3+i] = factory[5-i];
    } else {
        memcpy(command+3, factory+commands[index].offset, commands[index].length);
    }
    return BTSTACK_CHIPSET_VALID_COMMAND;
}
void native_bt_factory_complete(uint16_t opcode, uint8_t status) {
    unsigned bit;
    switch (opcode) {
    case 0xfc1a: bit = 1; break;
    case 0xfc79: bit = 2; break;
    case 0xfc93: bit = 4; break;
    case 0xfc7a: bit = 8; break;
    default: return;
    }
    printf("FACTORY_COMMAND opcode=0x%04x status=%u\n", opcode, status);
    if (status) failed = 1;
    else completed |= bit;
}
int native_bt_factory_verified(const bd_addr_t address) {
    return !failed && completed == 15 && memcmp(address, factory, 6) == 0;
}
static const btstack_chipset_t chipset = {
    .name = "Original X620 MTK volatile calibration", .init = init, .next_command = next_command,
};
const btstack_chipset_t *native_bt_factory_chipset(void) { return &chipset; }
