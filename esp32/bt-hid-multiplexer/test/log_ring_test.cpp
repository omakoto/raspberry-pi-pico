// Host-side test of the console log ring (src/log_ring.cpp) with fake sinks.
#include <stdio.h>
#include <string.h>
#include <string>
#include "log_ring.h"

static std::string g_uart, g_cdc;
static uint32_t g_uart_space = 32, g_cdc_space = 64, g_now = 0;
static bool g_cdc_ready = true;

static uint32_t uart_space() { return g_uart_space; }
static void uart_write(const uint8_t *d, uint32_t n) { g_uart.append((const char *)d, n); }
static bool cdc_ready() { return g_cdc_ready; }
static uint32_t cdc_space() { return g_cdc_space; }
static void cdc_write(const uint8_t *d, uint32_t n) { g_cdc.append((const char *)d, n); }
static void pump() { g_now += 10; }  // time passes while flush() waits
static uint32_t now_ms() { return g_now; }

static const LogRing::Sinks sinks = {uart_space, uart_write, cdc_ready, cdc_space, cdc_write, pump, now_ms};

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void drain_all() { for (int i = 0; i < 10000; i++) LogRing::drain(); }
static void clear() { g_uart.clear(); g_cdc.clear(); g_cdc_ready = true; g_uart_space = 32; g_cdc_space = 64; }

int main() {
    // Text reaches both sinks, with lone newlines turned into CRLF and CRLF left alone.
    LogRing::init(&sinks, false);
    LogRing::write("one\ntwo\r\nthree", 14);
    drain_all();
    CHECK(g_uart == "one\r\ntwo\r\nthree");
    CHECK(g_cdc == g_uart);

    // Nothing blocks when a sink has no room, and nothing is lost meanwhile.
    clear();
    LogRing::init(&sinks, false);
    g_uart_space = 0;
    g_cdc_space = 0;
    LogRing::write("pending\n", 8);
    drain_all();
    CHECK(g_uart.empty() && g_cdc.empty());
    g_uart_space = 32;
    g_cdc_space = 64;
    drain_all();
    CHECK(g_uart == "pending\r\n" && g_cdc == "pending\r\n");

    // The USB serial port keeps its backlog until a terminal opens it.
    clear();
    LogRing::init(&sinks, false);
    g_cdc_ready = false;
    LogRing::write("early\n", 6);
    drain_all();
    CHECK(g_uart == "early\r\n" && g_cdc.empty());
    g_cdc_ready = true;
    drain_all();
    CHECK(g_cdc == "early\r\n");

    // Writing more than the ring holds while the consumers stand still drops the oldest text and
    // says so, instead of blocking or corrupting anything.
    clear();
    LogRing::init(&sinks, false);
    g_uart_space = 0;
    g_cdc_ready = false;
    std::string big;
    for (int i = 0; i < LOG_RING_SIZE / 3; i++) big += "line" + std::to_string(i % 10) + "\n";
    LogRing::write(big.data(), big.size());
    g_uart_space = 32;
    drain_all();
    CHECK(g_uart.find("bytes dropped") != std::string::npos);
    CHECK(g_uart.size() > (size_t)LOG_RING_SIZE / 2);

    // A single write larger than the ring keeps its tail.
    clear();
    LogRing::init(&sinks, false);
    std::string huge(LOG_RING_SIZE * 2, 'a');
    huge += "END";
    LogRing::write(huge.data(), huge.size());
    drain_all();
    CHECK(g_uart.find("aaaEND") != std::string::npos);                 // the tail is intact
    CHECK(g_uart.find("bytes dropped") != std::string::npos);          // and the lost head is reported

    // flush() gives up after its timeout even if a sink never accepts anything.
    clear();
    LogRing::init(&sinks, false);
    g_uart_space = 0;
    LogRing::write("stuck\n", 6);
    uint32_t t0 = g_now;
    LogRing::flush(100);
    CHECK(g_now - t0 >= 100 && g_now - t0 < 200);

    // writeLong() delivers text longer than the ring.
    clear();
    LogRing::init(&sinks, false);
    std::string longtext;
    for (int i = 0; i < 3000; i++) longtext += "x" + std::to_string(i) + "\n";
    LogRing::writeLong(longtext.data(), longtext.size());
    LogRing::flush(1000000);
    CHECK(g_uart.find("x2999\r\n") != std::string::npos);
    CHECK(g_uart.find("bytes dropped") == std::string::npos);

    // After a "reboot" (init again with the ring memory intact) the previous run is readable:
    // its log, breadcrumbs in order, stage and uptime.
    clear();
    LogRing::init(&sinks, false);
    LogRing::write("last words\n", 11);
    for (uint16_t i = 1; i <= 20; i++) LogRing::breadcrumb(0x1000 + i);
    LogRing::stage(5);
    LogRing::heartbeat(12345);
    LogRing::init(&sinks, true);
    CHECK(LogRing::previousRunValid() && LogRing::previousRunWasWatchdog());
    uint32_t len = 0;
    const uint8_t *log = LogRing::previousLog(&len);
    CHECK(len == 11 && memcmp(log, "last words\n", 11) == 0);
    uint16_t crumbs[LOG_CRUMB_COUNT];
    uint8_t n = LogRing::previousBreadcrumbs(crumbs);
    CHECK(n == LOG_CRUMB_COUNT && crumbs[0] == 0x1005 && crumbs[n - 1] == 0x1014);
    CHECK(LogRing::previousStage() == 5 && LogRing::previousUptimeMs() == 12345);

    // The new run starts with an empty log and does not see the old text as pending output.
    clear();
    drain_all();
    CHECK(g_uart.empty());

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All log ring tests passed\n");
    return 0;
}
