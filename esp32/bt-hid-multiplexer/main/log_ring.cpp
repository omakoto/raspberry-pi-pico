#include "log_ring.h"
#include <stdio.h>
#include <string.h>
#include "platform.h"

// Contents survive a watchdog reset because the section is not cleared at startup.
struct PersistentLog {
    uint32_t magic;
    uint32_t write_pos;      // total bytes written since init (the ring holds the last LOG_RING_SIZE)
    uint32_t uptime_ms;      // last heartbeat
    uint16_t stage;          // handler the bt_app task is running
    uint16_t crumb_index;
    uint16_t crumbs[LOG_CRUMB_COUNT];
    uint8_t buf[LOG_RING_SIZE];
};
static const uint32_t LOG_MAGIC = 0x4C4F4752;  // 'LOGR'

static PersistentLog s_log PLATFORM_NOINIT;

static const LogRing::Sinks *s_sinks = nullptr;
static uint32_t s_uart_pos = 0, s_cdc_pos = 0;
static bool s_uart_last_cr = false, s_cdc_last_cr = false;

// The previous run, copied out of the ring at startup because the ring is reused.
static bool s_prev_valid = false;
static bool s_prev_watchdog = false;
static uint8_t s_prev_log[LOG_RING_SIZE];
static uint32_t s_prev_len = 0;
static uint32_t s_prev_uptime_ms = 0;
static uint16_t s_prev_stage = 0;
static uint16_t s_prev_crumbs[LOG_CRUMB_COUNT];
static uint8_t s_prev_crumb_count = 0;

void LogRing::init(const Sinks *sinks, bool was_watchdog_reset) {
    s_sinks = sinks;

    s_prev_valid = false;
    s_prev_watchdog = was_watchdog_reset;
    if (s_log.magic == LOG_MAGIC && s_log.crumb_index < 0xFFF0) {
        s_prev_valid = true;
        uint32_t wp = s_log.write_pos;
        s_prev_len = (wp < LOG_RING_SIZE) ? wp : LOG_RING_SIZE;
        for (uint32_t i = 0; i < s_prev_len; i++) {
            s_prev_log[i] = s_log.buf[(wp - s_prev_len + i) % LOG_RING_SIZE];
        }
        s_prev_uptime_ms = s_log.uptime_ms;
        s_prev_stage = s_log.stage;
        uint16_t n = (s_log.crumb_index < LOG_CRUMB_COUNT) ? s_log.crumb_index : LOG_CRUMB_COUNT;
        for (uint16_t i = 0; i < n; i++) {
            s_prev_crumbs[i] = s_log.crumbs[(s_log.crumb_index - n + i) % LOG_CRUMB_COUNT];
        }
        s_prev_crumb_count = (uint8_t)n;
    }

    memset(&s_log, 0, sizeof(s_log));
    s_log.magic = LOG_MAGIC;
    s_uart_pos = s_cdc_pos = 0;
    s_uart_last_cr = s_cdc_last_cr = false;
}

void LogRing::write(const char *data, size_t len) {
    if (!data || len == 0) return;
    platform_critical_enter();
    if (len > LOG_RING_SIZE) {
        // Only the tail can be kept. The position still advances over the discarded head, so that a
        // consumer that was reading it notices that it has been lapped and reports the loss.
        s_log.write_pos += (uint32_t)(len - LOG_RING_SIZE);
        data += len - LOG_RING_SIZE;
        len = LOG_RING_SIZE;
    }
    uint32_t start = s_log.write_pos % LOG_RING_SIZE;
    size_t first = LOG_RING_SIZE - start;
    if (first > len) first = len;
    memcpy(&s_log.buf[start], data, first);
    if (len > first) memcpy(&s_log.buf[0], data + first, len - first);
    s_log.write_pos += (uint32_t)len;
    platform_critical_exit();
}

// Copies pending ring bytes into out, turning a lone '\n' into "\r\n". Returns the output length and
// advances *pos past what was consumed; never produces more than max bytes.
static uint32_t take(uint32_t *pos, bool *last_cr, uint8_t *out, uint32_t max) {
    uint32_t wp = s_log.write_pos;
    uint32_t n = 0;
    while (*pos != wp && n < max) {
        uint8_t c = s_log.buf[*pos % LOG_RING_SIZE];
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
static uint32_t resync(uint32_t *pos) {
    uint32_t wp = s_log.write_pos;
    if (wp - *pos > LOG_RING_SIZE) {
        uint32_t lost = wp - LOG_RING_SIZE - *pos;
        *pos = wp - LOG_RING_SIZE;
        return lost;
    }
    return 0;
}

// The console task drains the ring, but another task can drain it as well while it waits for its
// output to go out (flush() before a reboot, writeLong()). Only one of them may move the consumer
// positions at a time; the other one simply skips its turn.
static bool s_draining = false;

static bool begin_drain() {
    platform_critical_enter();
    bool ok = !s_draining;
    if (ok) s_draining = true;
    platform_critical_exit();
    return ok;
}

static void end_drain() {
    s_draining = false;
}

void LogRing::drain() {
    if (!s_sinks) return;
    if (!begin_drain()) return;
    uint8_t chunk[96];

    uint32_t lost = resync(&s_uart_pos);

    uint32_t space = s_sinks->uart_space();
    if (space > sizeof(chunk)) space = sizeof(chunk);
    if (space > 0) {
        uint32_t n = take(&s_uart_pos, &s_uart_last_cr, chunk, space);
        if (n > 0) s_sinks->uart_write(chunk, n);
    }

    // The USB serial port keeps its backlog until a terminal opens it (within the ring's size).
    if (s_sinks->cdc_ready()) {
        resync(&s_cdc_pos);
        space = s_sinks->cdc_space();
        if (space > sizeof(chunk)) space = sizeof(chunk);
        if (space > 0) {
            uint32_t n = take(&s_cdc_pos, &s_cdc_last_cr, chunk, space);
            if (n > 0) s_sinks->cdc_write(chunk, n);
        }
    } else {
        resync(&s_cdc_pos);
    }

    end_drain();

    if (lost > 0) {
        char msg[48];
        int len = snprintf(msg, sizeof(msg), "\n[log: %lu bytes dropped]\n", (unsigned long)lost);
        if (len > 0) write(msg, (size_t)len);
    }
}

static bool all_drained() {
    if (s_uart_pos != s_log.write_pos) return false;
    return !(s_sinks && s_sinks->cdc_ready()) || s_cdc_pos == s_log.write_pos;
}

void LogRing::flush(uint32_t timeout_ms) {
    if (!s_sinks) return;
    uint32_t start = s_sinks->now_ms();
    while (!all_drained() && s_sinks->now_ms() - start < timeout_ms) {
        if (s_sinks->pump) s_sinks->pump();
        drain();
    }
}

void LogRing::writeLong(const char *data, size_t len) {
    const size_t step = 512;
    while (len > 0) {
        size_t n = (len > step) ? step : len;
        write(data, n);
        data += n;
        len -= n;
        flush(500);
    }
}

void LogRing::breadcrumb(uint16_t code) {
    platform_critical_enter();
    s_log.crumbs[s_log.crumb_index % LOG_CRUMB_COUNT] = code;
    s_log.crumb_index++;
    platform_critical_exit();
}

void LogRing::stage(uint16_t stage) {
    s_log.stage = stage;
}

void LogRing::heartbeat(uint32_t uptime_ms) {
    s_log.uptime_ms = uptime_ms;
}

bool LogRing::previousRunValid() { return s_prev_valid; }
bool LogRing::previousRunWasWatchdog() { return s_prev_watchdog; }
const uint8_t *LogRing::previousLog(uint32_t *len) {
    *len = s_prev_valid ? s_prev_len : 0;
    return s_prev_log;
}
uint32_t LogRing::previousUptimeMs() { return s_prev_uptime_ms; }
uint16_t LogRing::previousStage() { return s_prev_stage; }
uint8_t LogRing::previousBreadcrumbs(uint16_t out[LOG_CRUMB_COUNT]) {
    memcpy(out, s_prev_crumbs, sizeof(s_prev_crumbs));
    return s_prev_crumb_count;
}
