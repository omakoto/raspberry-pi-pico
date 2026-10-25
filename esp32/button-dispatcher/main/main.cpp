#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dispatcher/api.hpp"
#include "dispatcher/app.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_scanner.hpp"
#include "buzzer.hpp"
#include "config_manager.hpp"
#include "dual_logger.hpp"
#include "esp_platform.hpp"
#include "mdns_service.hpp"
#include "status_led.hpp"
#include "usb_msc.hpp"
#include "web_server.hpp"
#include "wifi_manager.hpp"

static const char* TAG = "Main";

// The clock matters for HTTPS certificate checks, for signing SwitchBot requests and for log times.
// Until it is set, SwitchBot calls fail with "the clock is not set yet", so the log records when that ends.
static dispatcher::AppCore* g_app_for_clock_log = nullptr;

static void on_clock_synced(struct timeval* tv) {
    char text[32];
    struct tm utc;
    gmtime_r(&tv->tv_sec, &utc);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", &utc);
    ESP_LOGI(TAG, "Clock set from NTP: %s", text);
    // Only the first sync goes to the log page; the periodic re-syncs are not interesting there.
    static bool first = true;
    if (first && g_app_for_clock_log != nullptr) {
        first = false;
        std::lock_guard<std::mutex> lock(g_app_for_clock_log->mutex);
        g_app_for_clock_log->log.add("info", std::string("Clock set from NTP: ") + text);
    }
}

static void start_time_sync(dispatcher::AppCore* app) {
    g_app_for_clock_log = app;
    esp_sntp_set_time_sync_notification_cb(on_clock_synced);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
}

// The forwarding task: sends reports whenever forward_once() says one is due, woken early when a press is
// queued. Reports are plain HTTP, so this needs less stack than the action worker, which also does TLS.
constexpr int kForwardStackBytes = 8 * 1024;

[[noreturn]] static void forward_loop(dispatcher::AppCore& app, StatusLed& led) {
    while (true) {
        const double wait_s = app.forward_once();
        bool link_down = false;
        {
            std::lock_guard<std::mutex> lock(app.mutex);
            link_down = app.device.role == dispatcher::Role::kSatellite && app.forwarder.failing();
        }
        led.set_link_down(link_down);
        if (wait_s > 0) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(static_cast<uint32_t>(wait_s * 1000) + 1));
    }
}

extern "C" void app_main() {
    // 1. Logging to both UART0 and the USB serial port
    dual_logger_init();
    ESP_LOGI(TAG, "Starting BLE button dispatcher...");

    // 2. NVS (needed by Wi-Fi and the Bluetooth controller)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing and re-initializing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // TCP/IP stack and default system event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Read config.toml from the config drive, then hand the drive over to the USB driver
    static ConfigManager config;
    if (config.init()) {
        config.load();
        config.unmount();
    } else {
        ESP_LOGW(TAG, "Running with compiled defaults (no config drive available)");
    }

    // 4. Status LED
    static StatusLed status_led(static_cast<gpio_num_t>(config.get_int("led_pin", 21)),
                                config.get_bool("led_active_low", true));
    status_led.init();
    status_led.set_state(LedState::INITIALIZING);

    // 4b. Acknowledgement buzzer
    static Buzzer buzzer(config.get_int("buzzer_pin", 9), config.get_bool("buzzer_active_low", true));
    buzzer.init();

    // 5. USB: serial console and the drag-and-drop config drive
    static UsbMsc usb_msc;
    usb_msc.init();

    // 6. The application: stored settings, buttons, bindings and the password
    static EspPlatform platform;
    platform.set_buzzer(&buzzer);
    if (!platform.init()) ESP_LOGE(TAG, "Platform start-up problem; settings may not be saved");
    static dispatcher::AppCore app(platform,
                                   config.get_int("pbkdf2_iterations", dispatcher::AuthService::kDefaultIterations));
    // The board's own keys (board.<mac>.hostname...) win over the defaults that every board shares.
    const std::string board_prefix = dispatcher::board_config_prefix(platform.device_mac());
    app.hostname_config.base = config.get_string(
        board_prefix + "hostname", config.get_string("hostname", dispatcher::kDefaultHostnameBase));
    app.hostname_config.append_mac = config.get_bool(
        board_prefix + "hostname_append_mac", config.get_bool("hostname_append_mac", true));
    std::string hostname_problem;
    dispatcher::effective_hostname(app.hostname_config, platform.device_mac(), &hostname_problem);
    app.http_port = config.get_int("http_port", 80);
    std::string init_error;
    app.init(&init_error);
    if (!init_error.empty()) ESP_LOGE(TAG, "%s", init_error.c_str());
    std::string hostname;
    dispatcher::Role role;
    {
        std::lock_guard<std::mutex> lock(app.mutex);
        hostname = app.hostname();
        role = app.device.role;
        app.log.add("info", std::string("Started as a ") + dispatcher::role_name(role));
        if (!init_error.empty()) app.log.add("error", init_error);
        if (!hostname_problem.empty()) app.log.add("error", "Bad " + hostname_problem);
    }
    if (!hostname_problem.empty()) ESP_LOGE(TAG, "Bad %s", hostname_problem.c_str());

    // 7. Wi-Fi with fallback access points
    std::vector<std::pair<std::string, std::string>> ap_list;
    const std::string primary_ssid = config.get_string("wifi_ssid", "");
    if (!primary_ssid.empty() && primary_ssid != "YOUR_WIFI_SSID") {
        ap_list.emplace_back(primary_ssid, config.get_string("wifi_password", ""));
    }
    for (int i = 1; i <= 9; ++i) {
        const std::string ssid = config.get_string("wifi_ssid" + std::to_string(i), "");
        if (!ssid.empty()) ap_list.emplace_back(ssid, config.get_string("wifi_password" + std::to_string(i), ""));
    }
    static WifiManager wifi(ap_list);
    wifi.init();
    wifi.set_hostname(hostname);
    xTaskCreate([](void*) { wifi.run(&status_led); }, "wifi_task", 4096, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    while (!wifi.is_connected()) vTaskDelay(pdMS_TO_TICKS(200));
    start_time_sync(&app);

    const int http_port = app.http_port;

    // 8. mDNS (for example dispatcher-aa-bb-cc-dd-ee-ff.local) and the web server
    static MdnsService mdns;
    mdns.init(hostname, http_port);
    static dispatcher::Api api(app);
    static WebServer web(api);
    web.start(http_port);

    // 8b. Forwarding to the dispatcher (satellite role); idle on a dispatcher, so a role change needs no restart
    static TaskHandle_t forward_task = nullptr;
    app.on_forward_queued = [] {
        if (forward_task != nullptr) xTaskNotifyGive(forward_task);
    };
    xTaskCreate([](void*) { forward_loop(app, status_led); }, "forwarder", kForwardStackBytes, nullptr, 4,
                &forward_task);

    // 9. Bluetooth scanning: presses feed the application core
    static BleScanner scanner(app, [] { status_led.pulse(150); });
    BleScanner::Settings ble;
    ble.interval_ms = config.get_int("ble_scan_interval_ms", ble.interval_ms);
    ble.window_ms = config.get_int("ble_scan_window_ms", ble.window_ms);
    ble.active = config.get_bool("ble_scan_active", ble.active);
    if (!scanner.start(ble)) {
        std::lock_guard<std::mutex> lock(app.mutex);
        app.log.add("error", "Bluetooth scanning could not be started");
    }

    status_led.set_state(LedState::READY);
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " BLE button dispatcher ready (%s)", dispatcher::role_name(role));
    ESP_LOGI(TAG, " Wi-Fi IP:  %s", wifi.get_ip_address().c_str());
    ESP_LOGI(TAG, " Web page:  http://%s.local:%d/", hostname.c_str(), http_port);
    ESP_LOGI(TAG, "=================================================");

    // Memory is the tightest resource (Wi-Fi, Bluetooth, TLS and the web server share about 340 KB), so the free
    // heap is logged now and every five minutes: a steady drop would point at a leak.
    while (true) {
        ESP_LOGI(TAG, "Free heap: %u bytes (lowest since boot: %u)", static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(esp_get_minimum_free_heap_size()));
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    }
}
