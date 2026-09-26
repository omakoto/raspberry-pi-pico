#include "dual_console.h"
#include "config.h"
#include "ble_hid_host.h"
#include "virtual_matrix.h"
#include "pico/bootrom.h"
#include "pico/time.h"
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

void dual_console_init() {
    uart_init(UART_PORT, UART_BAUDRATE);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_PORT, false, false);
    uart_set_format(UART_PORT, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_PORT, true);

    s_cdc_idx = 0;
    s_uart_idx = 0;
}

static void uart_write_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            uart_putc(UART_PORT, '\r');
        }
        uart_putc(UART_PORT, buf[i]);
    }
}

static void cdc_write_crlf(const char *buf, size_t len) {
    if (!tud_mounted() || !tud_cdc_n_connected(0) || len == 0) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            tud_cdc_n_write_char(0, '\r');
        }
        tud_cdc_n_write_char(0, buf[i]);
    }
    tud_cdc_n_write_flush(0);
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
        dual_println("Starting BLE discovery scan...");
        BleHidHost::startScan();
    } else if (strcmp(cmd, "stop") == 0) {
        dual_println("Stopping BLE scan.");
        BleHidHost::stopScan();
    } else if (strcmp(cmd, "status") == 0) {
        uint32_t ms = to_ms_since_boot(get_absolute_time());
        dual_printf("Status Report:\r\n");
        dual_printf("  Uptime:       %lu ms (%lu s)\r\n", ms, ms / 1000);
        dual_printf("  BLE State:    %s\r\n", BleHidHost::isConnected() ? "Connected" : "Disconnected");
        dual_printf("  Device:       %s\r\n", BleHidHost::getConnectedDeviceName());
        dual_printf("  Active Layer: %u\r\n", VirtualMatrix::getActiveLayer());
        dual_printf("  Scanning:     %s\r\n", BleHidHost::isScanning() ? "Active" : "Idle");
        dual_printf("  Passkey PIN:  %lu\r\n", (unsigned long)BleHidHost::getActivePasskey());
    } else if (strcmp(cmd, "reset") == 0) {
        dual_println("Clearing BLE bonds and resetting virtual matrix...");
        BleHidHost::clearBonds();
        VirtualMatrix::resetKeymap();
        dual_println("Factory reset complete.");
    } else if (strcmp(cmd, "help") == 0) {
        dual_println("Available Commands:");
        dual_println("  bootloader  - Reboot board directly into USB BOOTSEL ROM");
        dual_println("  pair / scan - Start 60-second BLE discovery pairing");
        dual_println("  stop        - Stop active BLE discovery scan");
        dual_println("  status      - Display connection status, layer, and uptime");
        dual_println("  reset       - Clear all BLE bonds and reset keymap to default");
        dual_println("  help        - Show this help summary");
    } else {
        dual_printf("Unknown command: '%s'. Type 'help' for command list.\r\n", cmd);
    }
}

void dual_console_update() {
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
