#include "log_ring.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

// Contents survive software, panic and watchdog resets because the section is not cleared at startup.
struct PersistentLog {
    uint32_t magic;
    uint32_t write_pos;   // total bytes written since init (the ring holds the last SIZE bytes)
    uint32_t uptime_ms;   // last heartbeat
    uint32_t magic_end;   // guards against a struct only partly retained
    uint8_t buf[LogRing::SIZE];
};
static constexpr uint32_t LOG_MAGIC = 0x4C4F4752;  // 'LOGR'
static constexpr uint32_t LOG_MAGIC_END = ~LOG_MAGIC;

static __NOINIT_ATTR PersistentLog s_log;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const LogRing::Sinks* s_sinks = nullptr;
static uint32_t s_uart_pos = 0;
static uint32_t s_cdc_pos = 0;
static bool s_uart_last_cr = false;
static bool s_cdc_last_cr = false;

// The previous run, copied out at startup because the ring is reused. Heap-allocated so that a
// power-on boot, which has no previous log, doesn't pay for it.
static char* s_prev_log = nullptr;
static size_t s_prev_len = 0;
static uint32_t s_prev_uptime_ms = 0;

// Copies the last len bytes (len <= SIZE) that end at write position wp, oldest first.
static void copy_tail(uint32_t wp, size_t len, char* out) {
    uint32_t start = (wp - len) % LogRing::SIZE;
    size_t first = LogRing::SIZE - start;
    if (first > len) first = len;
    std::memcpy(out, &s_log.buf[start], first);
    std::memcpy(out + first, &s_log.buf[0], len - first);
}

void LogRing::init(const Sinks* sinks) {
    s_sinks = sinks;

    // The magic numbers tell retained content from the random content of a powered-up SRAM.
    if (s_log.magic == LOG_MAGIC && s_log.magic_end == LOG_MAGIC_END) {
        uint32_t wp = s_log.write_pos;
        size_t len = (wp < SIZE) ? wp : SIZE;
        s_prev_log = static_cast<char*>(std::malloc(len + 1));
        if (s_prev_log != nullptr) {
            copy_tail(wp, len, s_prev_log);
            s_prev_log[len] = '\0';
            s_prev_len = len;
        }
        s_prev_uptime_ms = s_log.uptime_ms;
    }

    s_log.write_pos = 0;
    s_log.uptime_ms = 0;
    s_log.magic = LOG_MAGIC;
    s_log.magic_end = LOG_MAGIC_END;
    s_uart_pos = s_cdc_pos = 0;
    s_uart_last_cr = s_cdc_last_cr = false;
}

void LogRing::write(const char* data, size_t len) {
    if (data == nullptr || len == 0) return;
    portENTER_CRITICAL_SAFE(&s_lock);
    if (len > SIZE) {
        // Only the tail can be kept. The position still advances over the discarded head, so that a
        // consumer that was reading it notices that it has been lapped.
        s_log.write_pos += static_cast<uint32_t>(len - SIZE);
        data += len - SIZE;
        len = SIZE;
    }
    uint32_t start = s_log.write_pos % SIZE;
    size_t first = SIZE - start;
    if (first > len) first = len;
    std::memcpy(&s_log.buf[start], data, first);
    std::memcpy(&s_log.buf[0], data + first, len - first);
    s_log.write_pos += static_cast<uint32_t>(len);
    portEXIT_CRITICAL_SAFE(&s_lock);
}

// Copies pending ring bytes into out, turning a lone '\n' into "\r\n" for terminals. Returns the
// output length and advances *pos past what was consumed; never produces more than max bytes.
static size_t take(uint32_t* pos, bool* last_cr, uint8_t* out, size_t max) {
    uint32_t wp = s_log.write_pos;
    size_t n = 0;
    while (*pos != wp && n < max) {
        uint8_t c = s_log.buf[*pos % LogRing::SIZE];
        if (c == '\n' && !*last_cr) {
            if (n + 2 > max) break;
            out[n++] = '\r';
            out[n++] = '\n';
        } else {
            out[n++] = c;
        }
        *last_cr = (c == '\r');
        (*pos)++;
    }
    return n;
}

// Moves a consumer position forward if the writer has lapped it. Returns the number of bytes lost.
static uint32_t resync(uint32_t* pos) {
    uint32_t wp = s_log.write_pos;
    if (wp - *pos > LogRing::SIZE) {
        uint32_t lost = wp - LogRing::SIZE - *pos;
        *pos = wp - LogRing::SIZE;
        return lost;
    }
    return 0;
}

void LogRing::drain() {
    if (s_sinks == nullptr) return;
    uint8_t chunk[128];

    // The UART always transmits (nothing gates it), so falling behind here means real loss.
    uint32_t lost = resync(&s_uart_pos);
    size_t space = s_sinks->uart_space();
    if (space > sizeof(chunk)) space = sizeof(chunk);
    if (space > 0) {
        size_t n = take(&s_uart_pos, &s_uart_last_cr, chunk, space);
        if (n > 0) s_sinks->uart_write(chunk, n);
    }

    // The USB serial port keeps its backlog (up to the ring's size) until a terminal opens it.
    resync(&s_cdc_pos);
    if (s_sinks->cdc_ready()) {
        space = s_sinks->cdc_space();
        if (space > sizeof(chunk)) space = sizeof(chunk);
        if (space > 0) {
            size_t n = take(&s_cdc_pos, &s_cdc_last_cr, chunk, space);
            if (n > 0) s_sinks->cdc_write(chunk, n);
        }
    }

    if (lost > 0) {
        char msg[48];
        int len = std::snprintf(msg, sizeof(msg), "\n[log: %lu bytes dropped]\n", static_cast<unsigned long>(lost));
        if (len > 0) write(msg, static_cast<size_t>(len));
    }
}

void LogRing::heartbeat(uint32_t uptime_ms) {
    s_log.uptime_ms = uptime_ms;
}

std::string LogRing::snapshot() {
    // Copy in chunks so that interrupts are never held off for long.
    std::string out;
    out.reserve(SIZE);
    uint32_t wp = s_log.write_pos;
    uint32_t pos = (wp < SIZE) ? 0 : wp - SIZE;
    char chunk[512];
    while (true) {
        portENTER_CRITICAL(&s_lock);
        uint32_t now_wp = s_log.write_pos;
        // Text that has been overwritten while copying is skipped.
        if (now_wp - pos > SIZE) pos = now_wp - SIZE;
        size_t avail = now_wp - pos;
        // Stop at the position seen at the start so that a busy writer can't keep this going.
        size_t limit = (wp > pos) ? wp - pos : 0;
        size_t n = avail < limit ? avail : limit;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (n > 0) copy_tail(pos + n, n, chunk);
        portEXIT_CRITICAL(&s_lock);
        if (n == 0) break;
        out.append(chunk, n);
        pos += n;
    }
    return out;
}

bool LogRing::previous_valid() { return s_prev_log != nullptr; }

std::string LogRing::previous_log() {
    return s_prev_log != nullptr ? std::string(s_prev_log, s_prev_len) : std::string();
}

uint32_t LogRing::previous_uptime_ms() { return s_prev_uptime_ms; }
