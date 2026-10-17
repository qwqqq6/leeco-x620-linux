/* On-demand native Classic Bluetooth pairing and a read-only RFCOMM status service.
 * Numeric confirmation is explicit; link keys stay in a root-only TLV database.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "btstack_debug.h"
#include "bluetooth_psm.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "gap.h"
#include "hci.h"
#include "classic/rfcomm.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "classic/spp_server.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv_posix.h"
#include "classic/btstack_link_key_db_tlv.h"

#define RUN_DIR "/run/native-bluetooth"
#define KEY_DB "/var/lib/native-bluetooth/link-keys.tlv"
extern const hci_transport_t *native_stp_transport(void);
extern int native_bt_factory_load(void);
extern const btstack_chipset_t *native_bt_factory_chipset(void);
extern void native_bt_factory_complete(uint16_t opcode, uint8_t status);
extern int native_bt_factory_verified(const bd_addr_t address);

static btstack_packet_callback_registration_t registration;
static btstack_timer_source_t timer;
static btstack_tlv_posix_t tlv_context;
static volatile sig_atomic_t stopped;
static int pairing_mode, serving_mode, reconnect_mode, ready, shutting_down, pending, paired;
static int key_stored, bond_verified, failed;
static uint32_t pending_number;
static bd_addr_t pending_address;
static bd_addr_t reconnect_address;
static struct timespec started;
static double shutdown_at, request_at;
static uint8_t service[180];
static uint16_t rfcomm_channel;
static const char status_reply[] = "server=le620;os=Alpine Linux;transport=RFCOMM;status=OK\n";

static void on_signal(int signal_number) { stopped = signal_number; }
static double elapsed(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - started.tv_sec) + (now.tv_nsec - started.tv_nsec) / 1000000000.0;
}
static void clear_request(void) {
    unlink(RUN_DIR "/request");
    unlink(RUN_DIR "/accept");
    unlink(RUN_DIR "/deny");
}
static int sync_keys(void) {
    if (!tlv_context.file || fflush(tlv_context.file) || ferror(tlv_context.file) ||
        fsync(fileno(tlv_context.file))) {
        fputs("KEY_DB_SYNC_FAILED\n", stderr);
        failed = 1;
        return -1;
    }
    return 0;
}
static unsigned count_keys(void) {
    unsigned count = 0;
    btstack_link_key_iterator_t iterator;
    bd_addr_t address;
    link_key_t key;
    link_key_type_t key_type;
    if (gap_link_key_iterator_init(&iterator)) {
        while (gap_link_key_iterator_get_next(&iterator, address, key, &key_type)) {
            if (count == 0) memcpy(reconnect_address, address, sizeof address);
            count++;
        }
        gap_link_key_iterator_done(&iterator);
    }
    memset(key, 0, sizeof key);
    return count;
}
static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (type == RFCOMM_DATA_PACKET) {
        rfcomm_request_can_send_now_event(rfcomm_channel);
        return;
    }
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t address;
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_COMMAND_COMPLETE:
        if (size >= 6) native_bt_factory_complete((uint16_t)(packet[3] | packet[4] << 8), packet[5]);
        break;
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            gap_local_bd_addr(address);
            if (!native_bt_factory_verified(address)) {
                fputs("Factory address or calibration verification failed.\n", stderr);
                stopped = SIGTERM;
                break;
            }
            ready = 1;
            puts("CONTROLLER_READY factory_address_match=1 calibration_commands_ok=1");
            if (pairing_mode) {
                gap_connectable_control(1);
                gap_discoverable_control(1);
                puts("PAIRING_WINDOW_OPEN name=Le620-Linux seconds=180");
            } else if (serving_mode) {
                gap_connectable_control(1);
                gap_discoverable_control(0);
                puts("BONDED_SERVICE_READY discoverable=0 bondable=0");
            } else if (reconnect_mode) {
                uint16_t cid;
                /* Open only the standard SDP channel, then require encryption.
                 * No new pairing is allowed and no remote app is required. */
                uint8_t status = l2cap_create_channel(packet_handler, reconnect_address,
                                                     BLUETOOTH_PSM_SDP, 128, &cid);
                printf("BONDED_RECONNECT_START status=%u\n", status);
                if (status) { failed = 1; stopped = SIGTERM; }
            } else {
                gap_discoverable_control(0);
            }
        } else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF && shutting_down) {
            puts("CONTROLLER_OFF");
            btstack_run_loop_trigger_exit();
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, address);
        gap_pin_code_negative(address);
        puts("LEGACY_PIN_REJECTED");
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, address);
        if (!pairing_mode || paired || pending) {
            gap_ssp_confirmation_negative(address);
            break;
        }
        pending = 1;
        memcpy(pending_address, address, sizeof pending_address);
        pending_number = hci_event_user_confirmation_request_get_numeric_value(packet);
        request_at = elapsed();
        clear_request();
        FILE *request = fopen(RUN_DIR "/request", "w");
        if (request) {
            fprintf(request, "%06u\n", pending_number);
            fclose(request);
        }
        printf("NUMERIC_CONFIRMATION %06u\n", pending_number);
        break;
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        printf("PAIRING_COMPLETE status=%u\n", hci_event_simple_pairing_complete_get_status(packet));
        if (hci_event_simple_pairing_complete_get_status(packet) == 0) {
            paired = 1;
            gap_discoverable_control(0);
            /* Keep bonding enabled until the following Link Key Notification:
             * BTstack tests bondable before it persists that event's key. */
        }
        pending = 0;
        clear_request();
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        if (size >= 25 && paired) {
            link_key_t key;
            link_key_type_t key_type;
            hci_event_link_key_request_get_bd_addr(packet, address);
            int stored = gap_get_link_key_for_bd_addr(address, key, &key_type) &&
                         memcmp(key, packet + 8, sizeof key) == 0;
            memset(key, 0, sizeof key);
            if (stored && sync_keys() == 0) {
                key_stored = 1;
                printf("LINK_KEY_PERSISTED records=%u\n", count_keys());
            } else {
                fputs("LINK_KEY_PERSISTENCE_FAILED\n", stderr);
                failed = 1;
                stopped = SIGTERM;
            }
            gap_set_bondable_mode(0);
        }
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        printf("CONNECTION_COMPLETE status=%u\n", hci_event_connection_complete_get_status(packet));
        if (reconnect_mode && hci_event_connection_complete_get_status(packet) == 0)
            gap_request_security_level(hci_event_connection_complete_get_connection_handle(packet), LEVEL_3);
        break;
    case GAP_EVENT_SECURITY_LEVEL:
        printf("SECURITY_LEVEL level=%u status=%u\n", gap_event_security_level_get_security_level(packet),
               gap_event_security_level_get_status(packet));
        if (reconnect_mode && gap_event_security_level_get_status(packet) == 0 &&
            gap_event_security_level_get_security_level(packet) >= LEVEL_3) {
            bond_verified = 1;
            puts("SAVED_BOND_VERIFIED encrypted=1 mitm=1 new_pairing=0");
            stopped = SIGTERM;
        }
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        printf("ENCRYPTION_CHANGE status=%u enabled=%u\n",
               hci_event_encryption_change_get_status(packet),
               hci_event_encryption_change_get_encryption_enabled(packet));
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        printf("AUTHENTICATION_COMPLETE status=%u\n", packet[2]);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("DISCONNECTED reason=%u\n", packet[5]);
        break;
    case RFCOMM_EVENT_INCOMING_CONNECTION:
        rfcomm_channel = rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
        rfcomm_accept_connection(rfcomm_channel);
        break;
    case RFCOMM_EVENT_CHANNEL_OPENED:
        printf("RFCOMM_OPEN status=%u\n", rfcomm_event_channel_opened_get_status(packet));
        if (rfcomm_event_channel_opened_get_status(packet) == 0) {
            rfcomm_channel = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            rfcomm_request_can_send_now_event(rfcomm_channel);
        }
        break;
    case RFCOMM_EVENT_CAN_SEND_NOW:
        rfcomm_send(rfcomm_channel, (uint8_t *)status_reply, sizeof status_reply - 1);
        puts("RFCOMM_STATUS_SENT");
        break;
    case RFCOMM_EVENT_CHANNEL_CLOSED:
        rfcomm_channel = 0;
        puts("RFCOMM_CLOSED");
        break;
    case HCI_EVENT_HARDWARE_ERROR:
        fputs("CONTROLLER_HARDWARE_ERROR\n", stderr);
        stopped = SIGTERM;
        break;
    default:
        break;
    }
}
static void tick(btstack_timer_source_t *ts) {
    if (pending) {
        FILE *accept = fopen(RUN_DIR "/accept", "r");
        if (accept) {
            unsigned number;
            int matches = fscanf(accept, "%u", &number) == 1 && number == pending_number;
            fclose(accept);
            unlink(RUN_DIR "/accept");
            if (matches) {
                gap_ssp_confirmation_response(pending_address);
                puts("NUMERIC_CONFIRMED");
                pending = 0;
                clear_request();
            }
        }
        if (access(RUN_DIR "/deny", F_OK) == 0 || elapsed() - request_at > 45) {
            gap_ssp_confirmation_negative(pending_address);
            pending = 0;
            clear_request();
            puts("NUMERIC_REJECTED");
        }
    }
    double limit = serving_mode ? (ready ? 0 : 20) : pairing_mode ? 180 : reconnect_mode ? 35 : (ready ? 3 : 20);
    if (!shutting_down && (stopped || (limit && elapsed() > limit))) {
        shutting_down = 1;
        shutdown_at = elapsed();
        if (pending) gap_ssp_confirmation_negative(pending_address);
        pending = 0;
        clear_request();
        puts("SHUTTING_DOWN");
        hci_power_control(HCI_POWER_OFF);
    }
    if (shutting_down && elapsed() - shutdown_at > 10) {
        fputs("Forced transport closure after shutdown timeout.\n", stderr);
        failed = 1;
        btstack_run_loop_trigger_exit();
        return;
    }
    btstack_run_loop_set_timer(ts, 100);
    btstack_run_loop_add_timer(ts);
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--check") && strcmp(argv[1], "--pair") &&
                      strcmp(argv[1], "--serve") && strcmp(argv[1], "--reconnect")) || geteuid() != 0) {
        fputs("Usage (root): native-bluetooth --check|--pair|--serve|--reconnect\n", stderr);
        return 64;
    }
    pairing_mode = strcmp(argv[1], "--pair") == 0;
    serving_mode = strcmp(argv[1], "--serve") == 0;
    reconnect_mode = strcmp(argv[1], "--reconnect") == 0;
    umask(077);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((mkdir(RUN_DIR, 0700) < 0 && errno != EEXIST) ||
        (mkdir("/var/lib/native-bluetooth", 0700) < 0 && errno != EEXIST)) {
        perror("Bluetooth state directory"); return 1;
    }
    int lock = open("/run/native-bluetooth.lock", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        fputs("Bluetooth is already in use.\n", stderr); return 73;
    }
    FILE *pid_file = fopen(RUN_DIR "/pid", "w");
    if (!pid_file) { perror("Bluetooth PID file"); return 1; }
    fprintf(pid_file, "%ld\n", (long)getpid());
    fclose(pid_file);
    clear_request();
    if (native_bt_factory_load() < 0) { close(lock); return 1; }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    clock_gettime(CLOCK_MONOTONIC, &started);
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    hci_init(native_stp_transport(), NULL);
    hci_set_chipset(native_bt_factory_chipset());
    hci_enable_custom_pre_init();
    const btstack_tlv_t *tlv = btstack_tlv_posix_init_instance(&tlv_context, KEY_DB);
    if (!tlv_context.file || fchmod(fileno(tlv_context.file), 0600) || sync_keys()) return 1;
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, &tlv_context));
    unsigned records = count_keys();
    printf("KEY_DB_LOADED records=%u\n", records);
    if (reconnect_mode && records != 1) {
        fputs("Reconnect verification requires exactly one saved peer.\n", stderr);
        return 65;
    }
    registration.callback = packet_handler;
    hci_add_event_handler(&registration);
    l2cap_init();
    rfcomm_init();
    rfcomm_register_service(packet_handler, 1, 256);
    sdp_init();
    spp_create_sdp_record(service, sdp_create_service_record_handle(), 1, "Le620 Linux status");
    btstack_assert(de_get_len(service) <= sizeof service);
    sdp_register_service(service);
    gap_set_local_name("Le620-Linux");
    gap_set_class_of_device(0x010100);
    gap_set_security_level(LEVEL_3);
    gap_set_bondable_mode(pairing_mode);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    gap_discoverable_control(0);
    gap_connectable_control(0);
    btstack_run_loop_set_timer_handler(&timer, tick);
    btstack_run_loop_set_timer(&timer, 100);
    btstack_run_loop_add_timer(&timer);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    native_stp_transport()->close();
    sync_keys();
    btstack_tlv_posix_deinit(&tlv_context);
    if (tlv_context.file) fclose(tlv_context.file);
    clear_request();
    unlink(RUN_DIR "/pid");
    close(lock);
    printf("EXIT ready=%d paired=%d key_persisted=%d saved_bond_verified=%d failed=%d\n",
           ready, paired, key_stored, bond_verified, failed);
    return ready && !failed && (!pairing_mode || key_stored) && (!reconnect_mode || bond_verified) ? 0 : 1;
}
