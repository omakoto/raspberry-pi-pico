#include "dual_console.h"
#include "config.h"
#include "ble_hid_host.h"
#include "classic_hid_host.h"
#include "multiplexer.h"
#include "virtual_matrix.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char s_cdc_buf[128];
static size_t s_cdc_idx = 0;
static char s_uart_buf[128];
static size_t s_uart_idx = 0;

void reboot_to_bootsel() {
    dual_println("\r\n[System] Rebooting into USB BOOTSEL mode...");
    sleep_ms(50);
    tud_disconnect();
    sleep_ms(150);
    reset_usb_boot(0, 0);
}

// 1200 baud touch callback from TinyUSB CDC
extern "C" void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding) {
    if (itf == 0 && p_line_coding->bit_rate == 1200) {
        reboot_to_bootsel();
    }
}

static bool s_cdc_was_connected = false;
static bool s_pending_welcome = false;

// Tracks terminal DTR state to trigger welcome banner upon connection
extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == 0) {
        if (dtr && !s_cdc_was_connected) {
            s_cdc_was_connected = true;
            s_pending_welcome = true;
        } else if (!dtr) {
            s_cdc_was_connected = false;
        }
    }
}

// Writes text with CRLF line ending conversion to hardware UART0
static void uart_write_crlf(const char *buf, size_t len) {
    if (len == 0 || !buf) return;
    size_t last = 0;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            if (i > last) {
                uart_write_blocking(UART_PORT, (const uint8_t *)(buf + last), i - last);
            }
            uart_write_blocking(UART_PORT, (const uint8_t *)"\r\n", 2);
            last = i + 1;
        }
    }
    if (last < len) {
        uart_write_blocking(UART_PORT, (const uint8_t *)(buf + last), len - last);
    }
}

// Writes text with CRLF line ending conversion to USB CDC ACM serial console
static void cdc_write_crlf(const char *buf, size_t len) {
    if (!tud_mounted() || !tud_cdc_n_connected(0) || len == 0 || !buf) {
        return;
    }
    size_t last = 0;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            if (i > last) {
                tud_cdc_n_write(0, buf + last, (uint32_t)(i - last));
            }
            tud_cdc_n_write(0, "\r\n", 2);
            last = i + 1;
        }
    }
    if (last < len) {
        tud_cdc_n_write(0, buf + last, (uint32_t)(len - last));
    }
    tud_cdc_n_write_flush(0);
}

// Pico SDK stdio driver callbacks to route standard printf/puts to both consoles
static void stdio_dual_out_chars(const char *buf, int len) {
    if (len > 0 && buf) {
        uart_write_crlf(buf, (size_t)len);
        cdc_write_crlf(buf, (size_t)len);
    }
}

static void stdio_dual_out_flush(void) {
    if (tud_mounted() && tud_cdc_n_connected(0)) {
        tud_cdc_n_write_flush(0);
    }
}

static stdio_driver_t s_dual_stdio_driver = {
    .out_chars = stdio_dual_out_chars,
    .out_flush = stdio_dual_out_flush,
    .in_chars = NULL,
    .set_chars_available_callback = NULL,
    .next = NULL,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .last_ended_with_cr = false,
    .crlf_enabled = false
#endif
};

void dual_console_init() {
    uart_init(UART_PORT, UART_BAUDRATE);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_PORT, false, false);
    uart_set_format(UART_PORT, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_PORT, true);

    stdio_set_driver_enabled(&s_dual_stdio_driver, true);

    s_cdc_idx = 0;
    s_uart_idx = 0;
}

void dual_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        uart_write_crlf(buf, len);
        cdc_write_crlf(buf, len);
    }
}

void dual_println(const char *str) {
    if (!str) return;
    dual_printf("%s\r\n", str);
}

static void handle_command(const char *cmd) {
    // Skip empty lines
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return;

    dual_printf("[Console] Command received: %s\r\n", cmd);

    if (strcmp(cmd, "bootloader") == 0 || strcmp(cmd, "bootsel") == 0 || strcmp(cmd, "reboot bootloader") == 0) {
        reboot_to_bootsel();
    } else if (strcmp(cmd, "pair") == 0 || strcmp(cmd, "scan") == 0) {
        dual_println("Starting BLE pairing mode (60s)...");
        BleHidHost::startPairingMode();
    } else if (strcmp(cmd, "stop") == 0) {
        dual_println("Stopping BLE pairing mode and scan.");
        BleHidHost::stopPairingMode();
        BleHidHost::stopScan();
    } else if (strcmp(cmd, "status") == 0) {
        uint32_t ms = to_ms_since_boot(get_absolute_time());
        dual_printf("Status Report:\r\n");
        dual_printf("  Uptime:       %lu ms (%lu s)\r\n", ms, ms / 1000);
        dual_printf("  BLE Devices:  %u / %u connected\r\n", BleHidHost::getConnectedCount(), MAX_BLE_DEVICES);
        dual_printf("  Summary:      %s\r\n", BleHidHost::getConnectedDeviceName());
        dual_printf("  Active Layer: %u\r\n", VirtualMatrix::getActiveLayer());
        dual_printf("  Scanning:     %s\r\n", BleHidHost::isScanning() ? "Active" : "Idle");
        if (BleHidHost::isPairingMode()) {
            dual_printf("  Pairing Mode: Active (%lu s remaining)\r\n", (unsigned long)BleHidHost::getPairingModeRemainingSec());
        } else {
            dual_printf("  Pairing Mode: Inactive (bonded-only scan)\r\n");
        }
        dual_printf("  Passkey PIN:  %lu\r\n", (unsigned long)BleHidHost::getActivePasskey());
        BleHidHost::dumpDevices();
        ClassicHidHost::dumpDevices();
        BleHidHost::dumpBonds();
        ClassicHidHost::dumpBonds();
    } else if (strcmp(cmd, "devices") == 0) {
        BleHidHost::dumpDevices();
        ClassicHidHost::dumpDevices();
    } else if (strcmp(cmd, "bonds") == 0) {
        BleHidHost::dumpBonds();
        ClassicHidHost::dumpBonds();
    } else if (strncmp(cmd, "cconnect", 8) == 0) {
        int idx = -1;
        if (sscanf(cmd + 8, "%d", &idx) == 1 && idx >= 0) {
            ClassicHidHost::connectBonded((uint8_t)idx);
        } else {
            dual_println("Usage: cconnect <classic_bond_idx>   (indices from 'bonds', [cN])");
        }
    } else if (strncmp(cmd, "cqos", 4) == 0) {
        int slot = -1, type = 1;
        unsigned long latency_us = 0;
        if (sscanf(cmd + 4, "%d %d %lu", &slot, &type, &latency_us) >= 1 && slot >= 0) {
            ClassicHidHost::setQos((uint8_t)slot, (uint8_t)type, (uint32_t)latency_us);
        } else {
            dual_println("Usage: cqos <classic_slot> [service_type 0-2] [latency_us]");
        }
    } else if (strncmp(cmd, "cdisconnect", 11) == 0) {
        int slot = -1;
        if (sscanf(cmd + 11, "%d", &slot) == 1 && slot >= 0) {
            ClassicHidHost::disconnectSlot((uint8_t)slot);
        } else {
            dual_println("Usage: cdisconnect <classic_slot_idx>");
        }
    } else if (strcmp(cmd, "desc") == 0 || strcmp(cmd, "descriptor") == 0) {
        BleHidHost::dumpDescriptor();
    } else if (strncmp(cmd, "unbond", 6) == 0) {
        int idx = -1;
        if (sscanf(cmd + 6, "%d", &idx) == 1 && idx >= 0) {
            BleHidHost::unbond((uint8_t)idx);
        } else {
            dual_println("Usage: unbond <index>");
        }
    } else if (strncmp(cmd, "disconnect", 10) == 0 || strncmp(cmd, "kick", 4) == 0) {
        int slot = -1;
        const char *p = (cmd[0] == 'k') ? cmd + 4 : cmd + 10;
        if (sscanf(p, "%d", &slot) == 1 && slot >= 0) {
            BleHidHost::disconnectSlot((uint8_t)slot);
        } else {
            dual_println("Usage: disconnect <slot_idx>");
        }
    } else if (strncmp(cmd, "notif", 5) == 0) {
        int slot = -1;
        if (sscanf(cmd + 5, "%d", &slot) == 1 && slot >= 0) {
            BleHidHost::enableNotifications((uint8_t)slot);
        } else {
            for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
                BleHidHost::enableNotifications(i);
            }
        }
    } else if (strncmp(cmd, "getreport", 9) == 0) {
        int slot = -1, rep = 1;
        if (sscanf(cmd + 9, "%d %d", &slot, &rep) >= 1 && slot >= 0) {
            BleHidHost::sendGetReport((uint8_t)slot, (uint8_t)rep);
        } else {
            dual_println("Usage: getreport <slot_idx> [report_id]");
        }
    } else if (strncmp(cmd, "mode", 4) == 0) {
        int slot = -1, m = 1;
        if (sscanf(cmd + 4, "%d %d", &slot, &m) >= 1 && slot >= 0) {
            BleHidHost::sendSetProtocolMode((uint8_t)slot, (uint8_t)m);
        } else {
            dual_println("Usage: mode <slot_idx> [0=boot, 1=report]");
        }
    } else if (strncmp(cmd, "connparam", 9) == 0) {
        int slot = -1, latency = -1;
        float interval_ms = 0.0f;
        int n = sscanf(cmd + 9, "%d %d %f", &slot, &latency, &interval_ms);
        if (n >= 2 && slot >= 0 && latency >= 0) {
            uint16_t units = (interval_ms > 0.0f) ? (uint16_t)(interval_ms / 1.25f + 0.5f) : 0;
            BleHidHost::updateConnectionParams((uint8_t)slot, units, (uint16_t)latency);
        } else {
            dual_println("Usage: connparam <slot_idx> <slave_latency> [interval_ms]  (interval 7.5..4000, default: current)");
        }
    } else if (strncmp(cmd, "getmode", 7) == 0) {
        int slot = -1;
        if (sscanf(cmd + 7, "%d", &slot) == 1 && slot >= 0) {
            BleHidHost::requestProtocolMode((uint8_t)slot);
        } else {
            dual_println("Usage: getmode <slot_idx> (reads HID Protocol Mode)");
        }
    } else if (strncmp(cmd, "reports", 7) == 0) {
        const char *arg = cmd + 7;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
            BleHidHost::setReportLogging(true);
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
            BleHidHost::setReportLogging(false);
        } else {
            dual_println("Usage: reports on|off  (dump every incoming HID report)");
        }
    } else if (strncmp(cmd, "hcilog", 6) == 0) {
        const char *arg = cmd + 6;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
            BleHidHost::setHciPacketLogging(true);
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
            BleHidHost::setHciPacketLogging(false);
        } else {
            dual_println("Usage: hcilog on|off  (raw ACL/ATT packet dump; run 'stop' first)");
        }
    } else if (strncmp(cmd, "log", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        const char *arg = cmd + 3;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
            BleHidHost::setStackLogging(true);
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
            BleHidHost::setStackLogging(false);
        } else {
            dual_printf("BTstack log_info output is %s. Usage: log on|off\r\n",
                        BleHidHost::isStackLogging() ? "on" : "off");
        }
    } else if (strncmp(cmd, "authreq", 7) == 0) {
        // Pairing policy for new pairings: any combination of 'legacy'/'sc' and 'mitm'/'nomitm'.
        // No arguments just prints the current policy.
        const char *arg = cmd + 7;
        bool has_args = false;
        bool mitm = false;
        bool sc = false;
        bool ok = true;
        while (*arg) {
            while (*arg == ' ') arg++;
            if (*arg == '\0') break;
            has_args = true;
            if (strncmp(arg, "legacy", 6) == 0)      { sc = false; arg += 6; }
            else if (strncmp(arg, "nomitm", 6) == 0) { mitm = false; arg += 6; }
            else if (strncmp(arg, "mitm", 4) == 0)   { mitm = true; arg += 4; }
            else if (strncmp(arg, "sc", 2) == 0)     { sc = true; arg += 2; }
            else { ok = false; break; }
        }
        if (!ok) {
            dual_println("Usage: authreq [legacy|sc] [mitm|nomitm]   (no args: show current policy)");
            dual_println("  e.g. 'authreq legacy nomitm' (default), 'authreq sc', 'authreq legacy mitm'");
            dual_println("  Takes effect for new pairings only: 'unbond <idx>' the device, then re-pair it.");
        } else if (has_args) {
            BleHidHost::setAuthReq(mitm, sc);
        } else {
            BleHidHost::dumpAuthReq();
        }
    } else if (strncmp(cmd, "suspend", 7) == 0) {
        int slot = -1;
        if (sscanf(cmd + 7, "%d", &slot) == 1 && slot >= 0) {
            BleHidHost::sendExitSuspend((uint8_t)slot);
        } else {
            dual_println("Usage: suspend <slot_idx> (sends exit suspend)");
        }
    } else if (strncmp(cmd, "leds", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        if (*arg == '\0') {
            uint8_t cur = Multiplexer::getHostLeds();
            dual_printf("Host Lock LEDs: 0x%02X (Num=%d, Caps=%d, Scroll=%d)\r\n",
                        cur, (cur & 0x01) ? 1 : 0, (cur & 0x02) ? 1 : 0, (cur & 0x04) ? 1 : 0);
        } else {
            int val = 0;
            if (sscanf(arg, "%i", &val) == 1) {
                uint8_t mask = (uint8_t)val;
                Multiplexer::setHostLeds(mask);
                BleHidHost::sendHostLeds(mask);
                dual_printf("Host Lock LEDs set to 0x%02X (Num=%d, Caps=%d, Scroll=%d)\r\n",
                            mask, (mask & 0x01) ? 1 : 0, (mask & 0x02) ? 1 : 0, (mask & 0x04) ? 1 : 0);
            } else {
                dual_println("Usage: leds [mask]  (e.g. 'leds 2' for CapsLock, 'leds 0' for off)");
            }
        }
    } else if (strncmp(cmd, "mousespeed", 10) == 0 || strncmp(cmd, "speed", 5) == 0) {
        const char *arg = (cmd[0] == 'm') ? cmd + 10 : cmd + 5;
        while (*arg == ' ') arg++;
        if (*arg == '\0') {
            dual_printf("Global mouse speed: %u%%\r\n", BleHidHost::getGlobalMouseSpeed());
            for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
                dual_printf("  Slot %u: %u%%\r\n", i, BleHidHost::getMouseSpeed(i));
            }
        } else {
            int arg1 = -1, arg2 = -1;
            int count = sscanf(arg, "%d %d", &arg1, &arg2);
            if (count == 1 && arg1 > 0) {
                BleHidHost::setGlobalMouseSpeed((uint16_t)arg1);
                dual_printf("Set global mouse speed to %u%%\r\n", (uint16_t)arg1);
            } else if (count == 2 && arg1 >= 0 && arg1 < MAX_BLE_DEVICES && arg2 > 0) {
                BleHidHost::setMouseSpeed((uint8_t)arg1, (uint16_t)arg2);
                dual_printf("Set slot %d mouse speed to %u%%\r\n", arg1, (uint16_t)arg2);
            } else {
                dual_println("Usage: mousespeed [<percent>] | [<slot_idx> <percent>]");
                dual_println("  e.g. 'mousespeed 50' (set global), 'mousespeed 0 50' (set slot 0)");
            }
        }
    } else if (strcmp(cmd, "clearbonds") == 0) {
        dual_println("Clearing BLE bonds...");
        BleHidHost::clearBonds();
        dual_println("Bonds cleared.");
    } else if (strcmp(cmd, "resetkeymap") == 0) {
        VirtualMatrix::resetKeymap();
        dual_println("Keymap reset to defaults (bonds untouched).");
    } else if (strcmp(cmd, "reset") == 0) {
        dual_println("Clearing BLE bonds and resetting virtual matrix...");
        BleHidHost::clearBonds();
        VirtualMatrix::resetKeymap();
        dual_println("Factory reset complete.");
    } else if (strcmp(cmd, "help") == 0) {
        dual_println("Available Commands:");
        dual_println("  bootloader     - Reboot board directly into USB BOOTSEL ROM");
        dual_println("  pair / scan    - Start BLE pairing mode (60s window for new devices)");
        dual_println("  stop           - Stop active BLE pairing mode and discovery scan");
        dual_println("  status         - Display connection status, layer, and uptime");
        dual_println("  devices        - List all connected BLE devices and slot details");
        dual_println("  bonds          - Dump bonded peripheral database and cache (BLE and classic)");
        dual_println("  cconnect <n>   - Connect to classic bond [cN] from 'bonds' (host-initiated reconnect)");
        dual_println("  cdisconnect <n>- Drop classic Bluetooth HID slot n");
        dual_println("  cqos <n> [t] [us] - Experimental: set classic slot n link QoS (type t 0-2, latency in us)");
        dual_println("  desc           - Dump stored BLE HID report descriptor");
        dual_println("  mousespeed [..]- Get/set mouse sensitivity: [<percent>] or [<slot> <percent>]");
        dual_println("  notif [slot]   - Re-enable BLE HID notifications on slot(s)");
        dual_println("  getreport <s>  - Request HID Input report from slot <s>");
        dual_println("  mode <s> <m>   - Set HID protocol mode (0=boot, 1=report)");
        dual_println("  getmode <s>    - Read HID Protocol Mode from slot <s>");
        dual_println("  connparam <s> <lat> [ms] - Request LL connection params (slave latency, interval)");
        dual_println("  leds [mask]    - Show or set host Lock LED state (1=Num, 2=Caps, 4=Scroll)");
        dual_println("  suspend <s>    - Send HID Exit Suspend command to slot <s>");
        dual_println("  authreq [..]   - Show/set pairing policy: [legacy|sc] [mitm|nomitm]");
        dual_println("  log on|off     - Toggle BTstack internal log_info output");
        dual_println("  hcilog on|off  - Toggle raw HCI ACL/ATT packet dump (stop scan first)");
        dual_println("  reports on|off - Toggle dump of every incoming HID report");
        dual_println("  disconnect <s> - Disconnect link on slot <s>");
        dual_println("  unbond <idx>   - Remove bonded device index from table");
        dual_println("  clearbonds     - Clear all BLE bonds without resetting keymap");
        dual_println("  resetkeymap    - Reset the VIAL keymap to defaults, keeping bonds");
        dual_println("  reset          - Factory reset (clear bonds and reset keymap)");
        dual_println("  help           - Show this help summary");
    } else {
        dual_printf("Unknown command: '%s'. Type 'help' for command list.\r\n", cmd);
    }
}

void print_welcome_banner() {
#if PICO_RP2350
    const char *board_name = "Raspberry Pi Pico 2 W (RP2350)";
#else
    const char *board_name = "Raspberry Pi Pico W (RP2040)";
#endif

    dual_println("\r\n==================================================");
    dual_println("  Pico BLE HID Multiplexer");
    dual_printf ("  Hardware : %s\r\n", board_name);
    dual_printf ("  Firmware : v1.0.0 (Built %s %s)\r\n", __DATE__, __TIME__);
    dual_printf ("  Status   : %s\r\n", BleHidHost::isConnected() ? "Connected" : "Ready / Idle");
    dual_println("  Type 'help' for available console commands.");
    dual_println("==================================================\r\n");
}

void dual_console_update() {
    // Deliver welcome banner when a terminal opens the USB serial connection
    if (s_pending_welcome) {
        s_pending_welcome = false;
        print_welcome_banner();
    }

    // 1. Process USB CDC ACM input
    while (tud_cdc_n_available(0)) {
        char c = (char)tud_cdc_n_read_char(0);
        if (c == '\r' || c == '\n') {
            if (s_cdc_idx > 0) {
                s_cdc_buf[s_cdc_idx] = '\0';
                handle_command(s_cdc_buf);
                s_cdc_idx = 0;
            }
        } else if (c == '\b' || c == 0x7F) {
            if (s_cdc_idx > 0) s_cdc_idx--;
        } else if (s_cdc_idx < sizeof(s_cdc_buf) - 1) {
            s_cdc_buf[s_cdc_idx++] = c;
        }
    }

    // 2. Process Hardware UART0 input
    while (uart_is_readable(UART_PORT)) {
        char c = (char)uart_getc(UART_PORT);
        if (c == '\r' || c == '\n') {
            if (s_uart_idx > 0) {
                s_uart_buf[s_uart_idx] = '\0';
                handle_command(s_uart_buf);
                s_uart_idx = 0;
            }
        } else if (c == '\b' || c == 0x7F) {
            if (s_uart_idx > 0) s_uart_idx--;
        } else if (s_uart_idx < sizeof(s_uart_buf) - 1) {
            s_uart_buf[s_uart_idx++] = c;
        }
    }
}
