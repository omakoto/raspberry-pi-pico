#pragma once

#include <functional>

#include "dispatcher/app.hpp"

// Scans for BTHome advertisements with NimBLE (passive, receive only) and hands those that report a button
// press or a wheel turn to the application core.
//
// NimBLE's callback only recognizes BTHome service data and queues it; a worker task decodes it and calls
// AppCore::handle_packet, so the Bluetooth host task is never held up by locks or logging.
class BleScanner {
public:
    struct Settings {
        int interval_ms = 100;   // how often a scan window starts
        int window_ms = 90;      // how long each window listens (must not exceed the interval)
        bool active = false;     // active scans send requests, which only matters for advertised names
    };

    // `on_press_heard` is called (from the worker task) for each advertisement that carries a press or turn.
    BleScanner(dispatcher::AppCore& app, std::function<void()> on_press_heard);

    // Starts the Bluetooth controller, the NimBLE host and the worker task.
    bool start(const Settings& settings);

private:
    struct RawAdvert;

    static void host_task(void* arg);
    static void on_sync();
    static void on_reset(int reason);
    static int on_gap_event(struct ble_gap_event* event, void* arg);
    static void worker_entry(void* arg);

    void start_scan();
    void worker_loop();
    void handle_advert(const struct ble_gap_disc_desc& desc);

    dispatcher::AppCore& app_;
    std::function<void()> on_press_heard_;
    Settings settings_;
    void* queue_;  // a QueueHandle_t, kept opaque so this header needs no FreeRTOS includes
    uint32_t dropped_;
};
