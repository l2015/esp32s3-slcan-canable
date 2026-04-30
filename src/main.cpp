/*
 * ESP32-S3 SLCAN Firmware for HUD ECU Checker
 *
 * Port of CANable 2.5 SLCAN protocol to ESP32-S3 TWAI driver.
 * Compatible with HUD ECU Checker / ECU Hacker by ElmueSoft.
 *
 * Hardware: ESP32-S3 + SN65HVD230 CAN transceiver
 * - CAN TX: GPIO 4
 * - CAN RX: GPIO 5
 * - USB CDC on boot (enabled via board config)
 *
 * USB VID/PID: 0x16D0:0x117E (CANable vendor ID)
 * Configured in boards/esp32s3_slcan.json and platformio.ini
 *
 * Reference:
 * - https://github.com/Elmue/CANable-2.5-firmware-Slcan-and-Candlelight
 * - https://github.com/mintynet/esp32-slcan
 *
 * License: MIT
 */

#include "driver/twai.h"
#include "esp_timer.h"
#include "Arduino.h"

// ======================== CONFIGURATION ========================

#define CAN_TX_PIN        4
#define CAN_RX_PIN        5
#define SERIAL_BAUD       500000  // USB CDC baud rate (ignored for native USB)

// SLCAN Version (matches CANable 2.5 format)
#define SLCAN_VERSION     101
#define FIRMWARE_VERSION  "1.1.0"
#define SLCAN_VERSION_STR "101"

// Buffer sizes
#define CMD_BUF_SIZE      64
#define TX_QUEUE_SIZE     32
#define RX_QUEUE_SIZE     64
#define MAX_FILTERS       8

// ======================== DATA TYPES ==========================

typedef enum {
    USR_ErrorReport = 0x01,   // 'E' - CAN bus error reports
    USR_Feedback    = 0x02,   // 'F' - Command execution feedback
    USR_ReportTX    = 0x04,   // 'M' - Tx echo report with marker
    USR_Retransmit  = 0x08,   // 'A' - Auto re-transmit
    USR_DebugReport = 0x10,   // 'D' - String debug messages
    USR_SlcanDefault = USR_Retransmit | USR_Feedback, // Default: retransmit ON, feedback ON
} eUserFlags;

typedef enum {
    CAN_MODE_NORMAL = 0,
    CAN_MODE_LISTEN_ONLY,
} eCanMode;

// CAN filter structure
typedef struct {
    bool     active;
    bool     extended;
    uint32_t filter_id;
    uint32_t mask;
} can_filter_t;

// ======================== GLOBALS =============================

static volatile bool      bus_open = false;
static volatile eCanMode  can_mode = CAN_MODE_NORMAL;
static volatile uint8_t   user_flags = USR_SlcanDefault;
static volatile bool      timestamp_enabled = false;
static volatile bool      cr_enabled = false;
static volatile uint16_t  can_speed_kbps = 500;

// Error counters
static volatile uint8_t   tx_err_count = 0;
static volatile uint8_t   rx_err_count = 0;
static volatile uint8_t   bus_status = 0x00; // BUS_StatusActive

// Filters
static can_filter_t       filters[MAX_FILTERS];
static uint8_t            filter_count = 0;

// Command buffer
static char   cmd_buf[CMD_BUF_SIZE];
static int    cmd_idx = 0;

// Hex lookup
static const char hex_chars[] = "0123456789ABCDEF";

// ======================== HELPERS =============================

static void slcan_ack() {
    Serial.write('\r');
}

static void slcan_nack() {
    Serial.write('\a');
}

// Send feedback based on user flags
static void send_feedback(uint8_t code) {
    if (!(user_flags & USR_Feedback)) return;

    if (code == 0) {
        Serial.print("#\r"); // success
    } else {
        char buf[4] = { '#', (char)code, '\r', 0 };
        Serial.print(buf);
    }
}

// Send error report: "E<bus_status+proto_err><app_flags><tx_err><rx_err>\r"
static void send_error_report(uint8_t app_flags) {
    if (!(user_flags & USR_ErrorReport)) return;

    char buf[12];
    snprintf(buf, sizeof(buf), "E%02X%02X%02X%02X\r",
             (unsigned int)bus_status,
             (unsigned int)app_flags,
             (unsigned int)tx_err_count,
             (unsigned int)rx_err_count);
    Serial.print(buf);
}

// Send debug message: ">message\r"
static void send_debug(const char* msg) {
    if (!(user_flags & USR_DebugReport)) return;

    Serial.write('>');
    Serial.print(msg);
    Serial.write('\r');
}

// ======================== CAN DRIVER ==========================

static twai_timing_config_t get_timing_config(uint16_t kbps) {
    switch (kbps) {
        case 10:   return TWAI_TIMING_CONFIG_10KBITS();
        case 20:   return TWAI_TIMING_CONFIG_20KBITS();
        case 50:   return TWAI_TIMING_CONFIG_50KBITS();
        case 83:   return { .brp = 48, .tseg_1 = 13, .tseg_2 = 6, .sjw = 3, .triple_sampling = false };
        case 100:  return TWAI_TIMING_CONFIG_100KBITS();
        case 125:  return TWAI_TIMING_CONFIG_125KBITS();
        case 250:  return TWAI_TIMING_CONFIG_250KBITS();
        case 500:  return TWAI_TIMING_CONFIG_500KBITS();
        case 800:  return TWAI_TIMING_CONFIG_800KBITS();
        case 1000: return TWAI_TIMING_CONFIG_1MBITS();
        default:   return TWAI_TIMING_CONFIG_500KBITS();
    }
}

static void apply_tx_mode(twai_message_t* msg) {
    // TWAI retries automatically unless single-shot mode is requested.
    msg->ss = (user_flags & USR_Retransmit) ? 0 : 1;
}

static bool can_open_impl() {
    if (bus_open) return false; // already open

    twai_general_config_t g_config;
    if (can_mode == CAN_MODE_LISTEN_ONLY) {
        g_config = TWAI_GENERAL_CONFIG_DEFAULT(
            (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    } else {
        g_config = TWAI_GENERAL_CONFIG_DEFAULT(
            (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    }
    g_config.tx_queue_len = TX_QUEUE_SIZE;
    g_config.rx_queue_len = RX_QUEUE_SIZE;
    g_config.alerts_enabled = TWAI_ALERT_BUS_OFF |
                              TWAI_ALERT_BUS_RECOVERED |
                              TWAI_ALERT_ERR_PASS |
                              TWAI_ALERT_ERR_ACTIVE |
                              TWAI_ALERT_ABOVE_ERR_WARN |
                              TWAI_ALERT_BELOW_ERR_WARN |
                              TWAI_ALERT_RX_QUEUE_FULL;
    g_config.clkout_divider = 0;

    twai_timing_config_t t_config = get_timing_config(can_speed_kbps);

    twai_filter_config_t f_config;
    if (filter_count > 0) {
        // Use first active filter (TWAI supports 1 filter only)
        // For single filter: filter_code = filter_id, filter_mask = mask
        // Bits set to 1 in mask = must match, bits 0 = don't care
        bool found = false;
        for (int i = 0; i < MAX_FILTERS; i++) {
            if (filters[i].active && !filters[i].extended) {
                f_config.acceptance_code = filters[i].filter_id << 21;
                f_config.acceptance_mask = ~(filters[i].mask << 21);
                f_config.single_filter = true;
                found = true;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < MAX_FILTERS; i++) {
                if (filters[i].active && filters[i].extended) {
                    f_config.acceptance_code = filters[i].filter_id << 3;
                    f_config.acceptance_mask = ~(filters[i].mask << 3);
                    f_config.single_filter = true;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        }
    } else {
        f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    }

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) return false;

    err = twai_start();
    if (err != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }

    bus_open = true;
    bus_status = 0x00;
    tx_err_count = 0;
    rx_err_count = 0;
    return true;
}

static void can_close_impl() {
    if (!bus_open) return;
    twai_stop();
    twai_driver_uninstall();
    bus_open = false;
    bus_status = 0x00;
    tx_err_count = 0;
    rx_err_count = 0;
}

// ======================== SLCAN COMMAND PARSER =================

static bool parse_hex_nibble(char c, uint8_t* val) {
    if (c >= '0' && c <= '9') { *val = c - '0'; return true; }
    if (c >= 'A' && c <= 'F') { *val = c - 'A' + 10; return true; }
    if (c >= 'a' && c <= 'f') { *val = c - 'a' + 10; return true; }
    return false;
}

static bool parse_hex_bytes(const char* str, int start, int count, uint32_t* result) {
    *result = 0;
    for (int i = 0; i < count; i++) {
        uint8_t val;
        if (!parse_hex_nibble(str[start + i], &val)) return false;
        *result = (*result << 4) | val;
    }
    return true;
}

static void parse_slcan_command(char* buf, int len) {
    buf[len] = '\0';

    // Empty command
    if (len == 0) {
        send_feedback(0);
        return;
    }

    switch (buf[0]) {

        // ============ OPEN / CLOSE ============

        case 'O': // Open CAN channel
            if (len == 1) {
                can_mode = CAN_MODE_NORMAL;
            } else if (len == 2) {
                switch (buf[1]) {
                    case 'N': can_mode = CAN_MODE_NORMAL; break;
                    case 'S': can_mode = CAN_MODE_LISTEN_ONLY; break;
                    default: send_feedback('1'); return;
                }
            } else {
                send_feedback('2');
                return;
            }
            if (can_open_impl()) {
                send_feedback(0);
            } else {
                send_feedback('5'); // HAL error
            }
            break;

        case 'C': // Close CAN channel
            can_close_impl();
            can_mode = CAN_MODE_NORMAL;
            user_flags = USR_SlcanDefault;
            // CANable 2.5: Close never sends feedback
            break;

        // ============ BAUDRATE ============

        case 'S': // Set standard baudrate
            if (len == 2 && buf[1] >= '0' && buf[1] <= '9') {
                if (bus_open) { send_feedback('4'); return; } // must be closed
                static const uint16_t bauds[] = {10, 20, 50, 100, 125, 250, 500, 800, 1000, 83};
                uint8_t idx = buf[1] - '0';
                if (idx < 10) {
                    can_speed_kbps = bauds[idx];
                    send_feedback(0);
                } else {
                    send_feedback('2');
                }
            } else {
                send_feedback('2');
            }
            break;

        case 's': // Set custom baudrate: sBRP,SEG1,SEG2,SJW
            // ESP32 TWAI doesn't support arbitrary bit timing via SLCAN easily
            // Acknowledge but use nearest standard rate
            if (bus_open) { send_feedback('4'); return; }
            send_feedback('6'); // unsupported on ESP32
            break;

        // ============ TRANSMIT ============

        case 't': { // Transmit standard frame
            if (!bus_open) { send_feedback('3'); return; }
            if (len < 5) { send_feedback('2'); return; }

            twai_message_t msg = {};
            msg.extd = 0;
            msg.rtr = 0;
            apply_tx_mode(&msg);

            uint32_t id;
            if (!parse_hex_bytes(buf, 1, 3, &id)) { send_feedback('2'); return; }
            if (id > TWAI_STD_ID_MASK) { send_feedback('2'); return; }
            msg.identifier = id;

            uint32_t dlc;
            if (!parse_hex_bytes(buf, 4, 1, &dlc)) { send_feedback('2'); return; }
            if (dlc > 8) { send_feedback('2'); return; }
            msg.data_length_code = dlc;

            for (int i = 0; i < dlc; i++) {
                uint32_t b;
                if (!parse_hex_bytes(buf, 5 + i * 2, 2, &b)) { send_feedback('2'); return; }
                msg.data[i] = (uint8_t)b;
            }

            if (twai_transmit(&msg, pdMS_TO_TICKS(100)) == ESP_OK) {
                // Tx echo with marker
                if (user_flags & USR_ReportTX) {
                    char echo[32];
                    int pos = 0;
                    echo[pos++] = 't';
                    echo[pos++] = hex_chars[(msg.identifier >> 8) & 0xF];
                    echo[pos++] = hex_chars[(msg.identifier >> 4) & 0xF];
                    echo[pos++] = hex_chars[msg.identifier & 0xF];
                    echo[pos++] = hex_chars[msg.data_length_code];
                    for (int i = 0; i < msg.data_length_code; i++) {
                        echo[pos++] = hex_chars[msg.data[i] >> 4];
                        echo[pos++] = hex_chars[msg.data[i] & 0xF];
                    }
                    echo[pos++] = '\r';
                    echo[pos] = '\0';
                    Serial.print(echo);
                }
                send_feedback(0);
            } else {
                tx_err_count++;
                send_feedback('7');
                send_error_report(0x02); // APP_CanTxFail
            }
            break;
        }

        case 'T': { // Transmit extended frame
            if (!bus_open) { send_feedback('3'); return; }
            if (len < 10) { send_feedback('2'); return; }

            twai_message_t msg = {};
            msg.extd = 1;
            msg.rtr = 0;
            apply_tx_mode(&msg);

            uint32_t id;
            if (!parse_hex_bytes(buf, 1, 8, &id)) { send_feedback('2'); return; }
            if (id > TWAI_EXTD_ID_MASK) { send_feedback('2'); return; }
            msg.identifier = id;

            uint32_t dlc;
            if (!parse_hex_bytes(buf, 9, 1, &dlc)) { send_feedback('2'); return; }
            if (dlc > 8) { send_feedback('2'); return; }
            msg.data_length_code = dlc;

            for (int i = 0; i < dlc; i++) {
                uint32_t b;
                if (!parse_hex_bytes(buf, 10 + i * 2, 2, &b)) { send_feedback('2'); return; }
                msg.data[i] = (uint8_t)b;
            }

            if (twai_transmit(&msg, pdMS_TO_TICKS(100)) == ESP_OK) {
                if (user_flags & USR_ReportTX) {
                    char echo[48];
                    int pos = 0;
                    echo[pos++] = 'T';
                    for (int s = 7; s >= 0; s--) {
                        echo[pos++] = hex_chars[(msg.identifier >> (s * 4)) & 0xF];
                    }
                    echo[pos++] = hex_chars[msg.data_length_code];
                    for (int i = 0; i < msg.data_length_code; i++) {
                        echo[pos++] = hex_chars[msg.data[i] >> 4];
                        echo[pos++] = hex_chars[msg.data[i] & 0xF];
                    }
                    echo[pos++] = '\r';
                    echo[pos] = '\0';
                    Serial.print(echo);
                }
                send_feedback(0);
            } else {
                tx_err_count++;
                send_feedback('7');
                send_error_report(0x02);
            }
            break;
        }

        case 'r': { // Transmit standard RTR frame
            if (!bus_open) { send_feedback('3'); return; }
            if (len < 5) { send_feedback('2'); return; }

            twai_message_t msg = {};
            msg.extd = 0;
            msg.rtr = 1;
            apply_tx_mode(&msg);

            uint32_t id;
            if (!parse_hex_bytes(buf, 1, 3, &id)) { send_feedback('2'); return; }
            if (id > TWAI_STD_ID_MASK) { send_feedback('2'); return; }
            msg.identifier = id;

            uint32_t dlc;
            if (!parse_hex_bytes(buf, 4, 1, &dlc)) { send_feedback('2'); return; }
            if (dlc > 8) { send_feedback('2'); return; }
            msg.data_length_code = dlc;

            if (twai_transmit(&msg, pdMS_TO_TICKS(100)) == ESP_OK) {
                send_feedback(0);
            } else {
                tx_err_count++;
                send_feedback('7');
            }
            break;
        }

        case 'R': { // Transmit extended RTR frame
            if (!bus_open) { send_feedback('3'); return; }
            if (len < 10) { send_feedback('2'); return; }

            twai_message_t msg = {};
            msg.extd = 1;
            msg.rtr = 1;
            apply_tx_mode(&msg);

            uint32_t id;
            if (!parse_hex_bytes(buf, 1, 8, &id)) { send_feedback('2'); return; }
            if (id > TWAI_EXTD_ID_MASK) { send_feedback('2'); return; }
            msg.identifier = id;

            uint32_t dlc;
            if (!parse_hex_bytes(buf, 9, 1, &dlc)) { send_feedback('2'); return; }
            if (dlc > 8) { send_feedback('2'); return; }
            msg.data_length_code = dlc;

            if (twai_transmit(&msg, pdMS_TO_TICKS(100)) == ESP_OK) {
                send_feedback(0);
            } else {
                tx_err_count++;
                send_feedback('7');
            }
            break;
        }

        // ============ CAN FD (not supported on ESP32) ============

        case 'd': case 'D': case 'b': case 'B':
            send_feedback('6'); // unsupported
            break;

        case 'Y': case 'y':
            send_feedback('6'); // unsupported
            break;

        // ============ TIMESTAMP ============

        case 'Z':
            if (len == 2) {
                switch (buf[1]) {
                    case '0': timestamp_enabled = false; send_feedback(0); break;
                    case '1': timestamp_enabled = true;  send_feedback(0); break;
                    default: send_feedback('2'); break;
                }
            } else {
                send_feedback('2');
            }
            break;

        // ============ MODE FLAGS (CANable 2.5 extended) ============

        case 'M':
            if (len < 2) { send_feedback('2'); return; }
            for (int i = 1; i < len; i++) {
                switch (buf[i]) {
                    case 'A': // Enable auto retransmit
                        if (bus_open) { send_feedback('4'); return; }
                        user_flags |= USR_Retransmit;
                        break;
                    case 'a': // Disable auto retransmit
                        if (bus_open) { send_feedback('4'); return; }
                        user_flags &= ~USR_Retransmit;
                        break;
                    case 'E': // Enable error reports
                        user_flags |= USR_ErrorReport;
                        break;
                    case 'e': // Disable error reports
                        user_flags &= ~USR_ErrorReport;
                        break;
                    case 'F': // Enable feedback
                        user_flags |= USR_Feedback;
                        break;
                    case 'f': // Disable feedback
                        user_flags &= ~USR_Feedback;
                        break;
                    case 'M': // Enable Tx echo with marker
                        user_flags |= USR_ReportTX;
                        break;
                    case 'm': // Disable Tx echo with marker
                        user_flags &= ~USR_ReportTX;
                        break;
                    case 'D': // Enable debug messages
                        user_flags |= USR_DebugReport;
                        break;
                    case 'd': // Disable debug messages
                        user_flags &= ~USR_DebugReport;
                        break;
                    case '0': // Normal mode (legacy)
                        if (bus_open) { send_feedback('4'); return; }
                        can_mode = CAN_MODE_NORMAL;
                        break;
                    case '1': // Silent/listen-only mode (legacy)
                        if (bus_open) { send_feedback('4'); return; }
                        can_mode = CAN_MODE_LISTEN_ONLY;
                        break;
                    case 'I': // Identify (blink LED) - not implemented
                    case 'i': // Stop identify
                        break;
                    case 'R': // Enable termination (not applicable on most ESP32 boards)
                    case 'r': // Disable termination
                        send_feedback('6');
                        break;
                    default:
                        send_feedback('2');
                        return;
                }
            }
            send_feedback(0);
            break;

        // ============ AUTO RETRANSMIT (legacy) ============

        case 'A':
            if (len != 2) { send_feedback('2'); return; }
            if (bus_open) { send_feedback('4'); return; }
            switch (buf[1]) {
                case '0': user_flags &= ~USR_Retransmit; send_feedback(0); break;
                case '1': user_flags |= USR_Retransmit;  send_feedback(0); break;
                default: send_feedback('2'); break;
            }
            break;

        // ============ FILTERS / STATUS FLAGS ============

        case 'F':
            if (len == 1) {
                // Legacy: return status flags
                char flags[6];
                snprintf(flags, sizeof(flags), "F%02X\r", 0);
                Serial.print(flags);
                break;
            }
            // Set filter: F7E0,7FF;1F005000,1FFFFFFF
            if (bus_open) { send_feedback('4'); return; }

            filter_count = 0;
            memset(filters, 0, sizeof(filters));

            {
                int pos = 1;
                bool done = false;
                while (!done && filter_count < MAX_FILTERS) {
                    // Parse filter ID
                    int id_start = pos;
                    while (buf[pos] && buf[pos] != ',' && buf[pos] != ';') pos++;
                    int id_len = pos - id_start;
                    if (id_len != 3 && id_len != 8) { send_feedback('2'); return; }

                    if (buf[pos] != ',') { send_feedback('2'); return; }
                    pos++; // skip comma

                    // Parse mask
                    int mask_start = pos;
                    while (buf[pos] && buf[pos] != ';' && buf[pos] != '\0') pos++;
                    int mask_len = pos - mask_start;
                    if (mask_len != id_len) { send_feedback('2'); return; }

                    uint32_t filter_id, mask;
                    if (!parse_hex_bytes(buf, id_start, id_len, &filter_id) ||
                        !parse_hex_bytes(buf, mask_start, mask_len, &mask)) {
                        send_feedback('2');
                        return;
                    }

                    filters[filter_count].active = true;
                    filters[filter_count].extended = (id_len == 8);
                    filters[filter_count].filter_id = filter_id;
                    filters[filter_count].mask = mask;
                    filter_count++;

                    if (buf[pos] == ';') pos++; // skip semicolon
                    else done = true;
                }
            }
            send_feedback(0);
            break;

        case 'f': // Clear filters
            if (len == 1) {
                filter_count = 0;
                memset(filters, 0, sizeof(filters));
                send_feedback(0);
            } else {
                send_feedback('2');
            }
            break;

        // ============ BUS LOAD ============

        case 'L': // Enable bus load report
            // ESP32 TWAI doesn't easily provide bus load metrics
            // Acknowledge but report 0
            if (len >= 2) {
                send_feedback(0);
            } else {
                send_feedback('2');
            }
            break;

        // ============ VERSION / SERIAL / FLAGS ============

        case 'V': // Version
            if (len == 1) {
                char ver[256];
                snprintf(ver, sizeof(ver),
                    "+Board: ESP32-S3-SLCAN"
                    "\tMCU: ESP32-S3"
                    "\tDevID: 0"
                    "\tFirmware: " FIRMWARE_VERSION
                    "\tSlcan: " SLCAN_VERSION_STR
                    "\tClock: 80"
                    "\tChannels: 1"
                    "\tQuartz: No"
                    "\tLimits: 0,0,0,0,0,0,0,0\r");
                Serial.print(ver);
            } else {
                send_feedback('2');
            }
            break;

        case 'N': // Serial number
            if (len == 1) {
                uint64_t mac = ESP.getEfuseMac();
                uint16_t sn = (uint16_t)((mac >> 16) ^ (mac >> 32) ^ mac);
                char sn_buf[10];
                snprintf(sn_buf, sizeof(sn_buf), "N%04X\r", sn);
                Serial.print(sn_buf);
            } else {
                send_feedback('2');
            }
            break;

        // ============ DFU (not applicable on ESP32) ============

        case '*':
            if (strcmp(buf, "*DFU") == 0) {
                send_feedback('6'); // unsupported on ESP32
            } else if (strcmp(buf, "*Boot0:Off") == 0) {
                send_feedback('6');
            } else if (strcmp(buf, "*Boot0:?") == 0) {
                Serial.print("+0\r");
            } else {
                send_feedback('1');
            }
            break;

        // ============ NON-STANDARD HELPERS ============

        case 'l': // Toggle CR/LF
            cr_enabled = !cr_enabled;
            slcan_nack();
            break;

        case 'h': // Help
            Serial.println();
            Serial.println("ESP32-S3 SLCAN for HUD ECU Checker");
            Serial.println("v" FIRMWARE_VERSION " (Slcan " SLCAN_VERSION_STR ")");
            Serial.println();
            Serial.println("O     Open CAN (normal)");
            Serial.println("OS    Open CAN (listen-only)");
            Serial.println("C     Close CAN");
            Serial.println("S0-S9 Set baudrate");
            Serial.println("t     Send std frame");
            Serial.println("T     Send ext frame");
            Serial.println("r     Send std RTR");
            Serial.println("R     Send ext RTR");
            Serial.println("Z0/Z1 Timestamp off/on");
            Serial.println("V     Version info");
            Serial.println("N     Serial number");
            Serial.println("M...  Mode flags (ME/MF/MD/Ma/MF/Mf)");
            Serial.println("A0/A1 Auto retransmit");
            Serial.println("F     Set filter");
            Serial.println("f     Clear filters");
            Serial.println("L     Bus load report");
            Serial.println("d/D/b/B CAN FD (unsupported)");
            Serial.println("-----NON STANDARD-----");
            Serial.println("h     This help");
            Serial.println("l     Toggle CR");
            slcan_nack();
            break;

        // ============ UNKNOWN COMMAND ============

        default:
            send_feedback('1');
            break;
    }
}

// ======================== SERIAL / CAN BRIDGE ==================

static void transfer_serial_to_can() {
    int avail = Serial.available();
    if (avail <= 0) return;

    for (int i = 0; i < avail; i++) {
        char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (cmd_idx > 0) {
                cmd_buf[cmd_idx] = '\0';
                parse_slcan_command(cmd_buf, cmd_idx);
                cmd_idx = 0;
            }
        } else {
            if (cmd_idx < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_idx++] = c;
            } else {
                slcan_nack();
                cmd_idx = 0;
            }
        }
    }
}

static void transfer_can_to_serial() {
    if (!bus_open) return;

    twai_message_t rx_msg;
    if (twai_receive(&rx_msg, 0) != ESP_OK) return;

    // Build SLCAN frame string
    char frame[48];
    int pos = 0;

    if (rx_msg.extd) {
        // Extended frame: T + 8-digit ID + DLC + data
        if (rx_msg.rtr) {
            frame[pos++] = 'R';
        } else {
            frame[pos++] = 'T';
        }
        for (int s = 7; s >= 0; s--) {
            frame[pos++] = hex_chars[(rx_msg.identifier >> (s * 4)) & 0xF];
        }
        frame[pos++] = hex_chars[rx_msg.data_length_code];
    } else {
        // Standard frame: t + 3-digit ID + DLC + data
        if (rx_msg.rtr) {
            frame[pos++] = 'r';
        } else {
            frame[pos++] = 't';
        }
        frame[pos++] = hex_chars[(rx_msg.identifier >> 8) & 0xF];
        frame[pos++] = hex_chars[(rx_msg.identifier >> 4) & 0xF];
        frame[pos++] = hex_chars[rx_msg.identifier & 0xF];
        frame[pos++] = hex_chars[rx_msg.data_length_code];
    }

    // Data bytes
    for (int i = 0; i < rx_msg.data_length_code; i++) {
        frame[pos++] = hex_chars[rx_msg.data[i] >> 4];
        frame[pos++] = hex_chars[rx_msg.data[i] & 0xF];
    }

    // Timestamp (4 hex digits, milliseconds mod 60000)
    if (timestamp_enabled) {
        uint32_t ts = (esp_timer_get_time() / 1000) % 60000;
        frame[pos++] = hex_chars[(ts >> 12) & 0xF];
        frame[pos++] = hex_chars[(ts >> 8) & 0xF];
        frame[pos++] = hex_chars[(ts >> 4) & 0xF];
        frame[pos++] = hex_chars[ts & 0xF];
    }

    frame[pos++] = '\r';
    frame[pos] = '\0';

    Serial.print(frame);

    if (cr_enabled) {
        Serial.println();
    }
}

// ======================== SETUP & LOOP =========================

void setup() {
    Serial.begin(SERIAL_BAUD);
    // Don't block on DTR — some OBD2 scanners open port without asserting DTR
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) { delay(10); }

    // Initialize filters
    memset(filters, 0, sizeof(filters));
    filter_count = 0;
}

void loop() {
    if (bus_open) {
        transfer_can_to_serial();

        // Process TWAI alerts
        uint32_t alerts;
        if (twai_read_alerts(&alerts, pdMS_TO_TICKS(0)) == ESP_OK) {
            if (alerts & TWAI_ALERT_BUS_OFF) {
                bus_status = 0x30;
                send_error_report(0x00);
            }
            if (alerts & TWAI_ALERT_BUS_RECOVERED) {
                if (twai_start() == ESP_OK) {
                    bus_status = 0x00;
                    send_error_report(0x00);
                }
            }
            if (alerts & TWAI_ALERT_ERR_PASS) {
                bus_status = 0x20;
                send_error_report(0x00);
            }
            if (alerts & TWAI_ALERT_ABOVE_ERR_WARN) {
                bus_status = 0x10;
                send_error_report(0x00);
            }
            if ((alerts & TWAI_ALERT_ERR_ACTIVE) || (alerts & TWAI_ALERT_BELOW_ERR_WARN)) {
                bus_status = 0x00;
                send_error_report(0x00);
            }
            if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
                send_error_report(0x08); // APP_UsbInOverflow
            }
        }
    }
    transfer_serial_to_can();

    // Check for BusOff recovery
    if (bus_open && bus_status == 0x30) {
        // Try to recover: close and reopen
        // This is a simplified recovery - CANable 2.5 has more sophisticated handling
        static uint32_t last_recovery_attempt = 0;
        uint32_t now = millis();
        if (now - last_recovery_attempt > 5000) {
            last_recovery_attempt = now;
            twai_status_info_t status;
            twai_get_status_info(&status);
            if (status.state == TWAI_STATE_BUS_OFF) {
                // Attempt recovery
                twai_initiate_recovery();
            }
        }
    }

    // Periodic error state check
    static uint32_t last_error_check = 0;
    uint32_t now = millis();
    if (now - last_error_check > 1000) {
        last_error_check = now;
        if (bus_open) {
            twai_status_info_t status;
            if (twai_get_status_info(&status) == ESP_OK) {
                tx_err_count = status.tx_error_counter;
                rx_err_count = status.rx_error_counter;

                uint8_t new_status;
                if (status.state == TWAI_STATE_BUS_OFF) {
                    new_status = 0x30;
                } else if (tx_err_count > 128 || rx_err_count > 128) {
                    new_status = 0x20; // passive
                } else if (tx_err_count > 96 || rx_err_count > 96) {
                    new_status = 0x10; // warning
                } else {
                    new_status = 0x00; // active
                }

                if (new_status != bus_status) {
                    bus_status = new_status;
                    send_error_report(0x00);
                }
            }
        }
    }
}
