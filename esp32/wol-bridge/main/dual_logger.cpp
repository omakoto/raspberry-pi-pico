#include "dual_logger.hpp"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "log_ring.hpp"
#include "usb_msc.hpp"

static const char* TAG = "Console";

static constexpr uart_port_t CONSOLE_UART = UART_NUM_0;
static constexpr int CONSOLE_UART_RX_BUF = 512;
static constexpr int CONSOLE_UART_TX_BUF = 4096;
static constexpr uint32_t CONSOLE_POLL_MS = 10;
static constexpr size_t CONSOLE_LINE_MAX = 64;

static void (*s_status_callback)() = nullptr;

// Output sinks for LogRing. They never block: only as much as the UART driver's TX buffer or the
// USB serial FIFO can take right now is written.
static size_t sink_uart_space() {
    size_t space = 0;
    uart_get_tx_buffer_free_size(CONSOLE_UART, &space);
    return space;
}

static void sink_uart_write(const uint8_t* data, size_t len) {
    uart_write_bytes(CONSOLE_UART, data, len);
}

static bool sink_cdc_ready() {
    return tud_mounted() && tud_cdc_n_connected(0);
}

static size_t sink_cdc_space() {
    return tud_cdc_n_write_available(0);
}

static void sink_cdc_write(const uint8_t* data, size_t len) {
    tud_cdc_n_write(0, data, len);
    tud_cdc_n_write_flush(0);
}

static const LogRing::Sinks s_sinks = {
    sink_uart_space, sink_uart_write, sink_cdc_ready, sink_cdc_space, sink_cdc_write,
};

// stdout (printf from this firmware and from ESP-IDF components) and ESP_LOG* output go into the ring.
static int stdout_write(void* cookie, const char* buf, int len) {
    (void)cookie;
    if (len > 0 && buf != nullptr) {
        LogRing::write(buf, static_cast<size_t>(len));
    }
    return len;
}

static int log_vprintf(const char* fmt, va_list args) {
    char buf[320];
    int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        LogRing::write(buf, static_cast<size_t>(len) < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf) - 1);
    }
    return len;
}

static const char* reset_reason_str(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on (or RST button)";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "PANIC (crash)";
        case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
        case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
        case ESP_RST_WDT: return "WATCHDOG";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake-up";
        case ESP_RST_BROWNOUT: return "BROWNOUT (power supply dip)";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        default: return "unknown";
    }
}

std::string dual_logger_boot_report() {
    char buf[256];
    esp_reset_reason_t reason = esp_reset_reason();
    if (LogRing::previous_valid()) {
        uint32_t up_s = LogRing::previous_uptime_ms() / 1000;
        std::snprintf(buf, sizeof(buf),
                      "Reset reason: %s. The previous run was up for at least %lu s (%luh%02lum); "
                      "its last %u bytes of log were kept ('prev' command or GET /log).",
                      reset_reason_str(reason), static_cast<unsigned long>(up_s),
                      static_cast<unsigned long>(up_s / 3600), static_cast<unsigned long>(up_s / 60 % 60),
                      static_cast<unsigned>(LogRing::previous_log().size()));
    } else {
        std::snprintf(buf, sizeof(buf), "Reset reason: %s. No log of the previous run was kept.",
                      reset_reason_str(reason));
    }
    return buf;
}

void dual_logger_init() {
    LogRing::init(&s_sinks);

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;
    uart_driver_install(CONSOLE_UART, CONSOLE_UART_RX_BUF, CONSOLE_UART_TX_BUF, 0, nullptr, 0);
    uart_param_config(CONSOLE_UART, &uart_cfg);

    // Tasks copy the global stdout when they are created, so this must happen before any task of
    // this firmware starts.
    FILE* f = funopen(nullptr, nullptr, &stdout_write, nullptr, nullptr);
    setvbuf(f, nullptr, _IONBF, 0);
    _GLOBAL_REENT->_stdout = f;
    _GLOBAL_REENT->_stderr = f;
    stdout = f;
    stderr = f;
    esp_log_set_vprintf(&log_vprintf);

    esp_reset_reason_t reason = esp_reset_reason();
    bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                   reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;
    std::string report = dual_logger_boot_report();
    if (crashed) {
        ESP_LOGW(TAG, "%s", report.c_str());
    } else {
        ESP_LOGI(TAG, "%s", report.c_str());
    }
}

void dual_logger_set_status_callback(void (*callback)()) {
    s_status_callback = callback;
}

void dual_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vprintf(fmt, args);
    va_end(args);
}

void dual_println(const std::string& str) {
    LogRing::write(str.data(), str.size());
    LogRing::write("\n", 1);
}

// --- Console task ---------------------------------------------------------------------------------

enum class Port { UART, CDC };

// Writes long text (a log dump) straight to one port, bypassing the ring so that the dump neither
// evicts the current log nor goes to the other port. Waits for the port to take it, up to a timeout.
static void write_direct(Port port, const std::string& text) {
    const int64_t deadline_us = esp_timer_get_time() + 15 * 1000 * 1000;
    uint8_t chunk[128];
    size_t pos = 0;
    bool last_cr = false;
    while (pos < text.size()) {
        if (esp_timer_get_time() > deadline_us) return;
        size_t space;
        if (port == Port::UART) {
            space = sink_uart_space();
        } else {
            if (!sink_cdc_ready()) return;
            space = sink_cdc_space();
        }
        if (space > sizeof(chunk)) space = sizeof(chunk);
        size_t n = 0;
        while (pos < text.size() && n + 2 <= space) {
            char c = text[pos++];
            if (c == '\n' && !last_cr) chunk[n++] = '\r';
            chunk[n++] = static_cast<uint8_t>(c);
            last_cr = (c == '\r');
        }
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (port == Port::UART) {
            sink_uart_write(chunk, n);
        } else {
            sink_cdc_write(chunk, n);
        }
    }
}

// Gives the ports time to show the last messages before a reboot. Commands run on the console task,
// so nothing else drains the ring meanwhile.
static void drain_for(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += CONSOLE_POLL_MS) {
        LogRing::drain();
        vTaskDelay(pdMS_TO_TICKS(CONSOLE_POLL_MS));
    }
}

static void handle_command(Port port, const char* cmd) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return;

    if (std::strcmp(cmd, "help") == 0 || std::strcmp(cmd, "?") == 0) {
        dual_println("Commands:\n"
                     "  status      Uptime, Wi-Fi state, memory\n"
                     "  log         Show this run's log (the last 16 KB)\n"
                     "  prev        Show the log the previous run left (survives reboots and crashes)\n"
                     "  boot        Show the reset reason\n"
                     "  reboot      Restart the firmware (keeps the log for 'prev')\n"
                     "  bootloader  Reboot into the ROM download mode for flashing");
    } else if (std::strcmp(cmd, "status") == 0) {
        if (s_status_callback != nullptr) {
            s_status_callback();
        }
    } else if (std::strcmp(cmd, "log") == 0) {
        write_direct(port, "----- Log of this run -----\n" + LogRing::snapshot() + "----- End of log -----\n");
    } else if (std::strcmp(cmd, "prev") == 0) {
        if (LogRing::previous_valid()) {
            write_direct(port, "----- Log of the previous run -----\n" + LogRing::previous_log() +
                               "\n----- End of the previous run's log -----\n");
        } else {
            dual_println("No log of the previous run was kept (the board was powered up).");
        }
    } else if (std::strcmp(cmd, "boot") == 0) {
        dual_println(dual_logger_boot_report());
    } else if (std::strcmp(cmd, "reboot") == 0) {
        ESP_LOGW(TAG, "Rebooting by console command...");
        drain_for(200);
        esp_restart();
    } else if (std::strcmp(cmd, "bootloader") == 0) {
        ESP_LOGW(TAG, "Rebooting into download mode by console command...");
        drain_for(200);
        UsbMsc::reboot_to_download_mode();
    } else {
        dual_printf("Unknown command '%s'; type 'help'.\n", cmd);
    }
}

// Collects typed characters into a line and runs it on Enter. Echoes to the port it came from.
struct LineEditor {
    Port port;
    char buf[CONSOLE_LINE_MAX];
    size_t len;

    void echo(const char* s, size_t n) {
        if (port == Port::UART) {
            sink_uart_write(reinterpret_cast<const uint8_t*>(s), n);
        } else if (sink_cdc_ready()) {
            sink_cdc_write(reinterpret_cast<const uint8_t*>(s), n);
        }
    }

    void feed(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; i++) {
            char c = static_cast<char>(data[i]);
            if (c == '\r' || c == '\n') {
                if (len == 0 && c == '\n') continue;  // The '\n' of a "\r\n".
                echo("\r\n", 2);
                buf[len] = '\0';
                len = 0;
                handle_command(port, buf);
            } else if (c == '\b' || c == 0x7f) {
                if (len > 0) {
                    len--;
                    echo("\b \b", 3);
                }
            } else if (c >= 0x20 && len < CONSOLE_LINE_MAX - 1) {
                buf[len++] = c;
                echo(&c, 1);
            }
        }
    }
};

static void console_task(void* arg) {
    (void)arg;
    LineEditor uart_line = {Port::UART, {}, 0};
    LineEditor cdc_line = {Port::CDC, {}, 0};
    uint8_t in[32];

    while (true) {
        LogRing::drain();
        LogRing::heartbeat(static_cast<uint32_t>(esp_timer_get_time() / 1000));

        int n = uart_read_bytes(CONSOLE_UART, in, sizeof(in), 0);
        if (n > 0) uart_line.feed(in, static_cast<size_t>(n));

        if (tud_mounted() && tud_cdc_n_available(0) > 0) {
            uint32_t m = tud_cdc_n_read(0, in, sizeof(in));
            if (m > 0) cdc_line.feed(in, m);
        }

        vTaskDelay(pdMS_TO_TICKS(CONSOLE_POLL_MS));
    }
}

void dual_logger_start_console() {
    xTaskCreate(console_task, "console", 4096, nullptr, 3, nullptr);
}
