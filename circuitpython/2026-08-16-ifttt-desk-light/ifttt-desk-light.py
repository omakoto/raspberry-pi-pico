#!/usr/bin/env circuit-run
"""
IFTTT Webhook Trigger for Raspberry Pi Pico 2 W.

When a button connected to GP14 is pressed, this script connects to Wi-Fi
and makes an HTTP GET request to trigger the IFTTT desk_light event.

Pin Connections:
- GP14: Button -> Connected between GP14 and GND (Active LOW, internal pull-up)
- Common Ground: Pico GND pin
- Board LED: Visual indicator for Wi-Fi connection and request status
"""

import ssl
import time
import board
import digitalio
import wifi
import socketpool
import adafruit_requests

# Pin Definitions
PIN_BUTTON: board.Pin = board.GP14
PIN_LED: board.Pin = board.LED

# Wi-Fi Credentials
WIFI_SSID: str = "mguest"
WIFI_PASSWORD: str = "EmorY961"

# IFTTT Webhook configuration (using plain http to optimize speed and bypass SSL/TLS overhead)
IFTTT_URL: str = "http://maker.ifttt.com/trigger/desk_light/with/key/PKGiZBlTb19dy7sMz2cZQ"

# Debounce duration in seconds
DEBOUNCE_DELAY_SECONDS: float = 0.05


class DebouncedButton:

    def __init__(self, pin: board.Pin, debounce_delay_s: float = 0.05) -> None:
        self.io = digitalio.DigitalInOut(pin)
        self.io.direction = digitalio.Direction.INPUT
        self.io.pull = digitalio.Pull.UP
        self.debounce_delay_s: float = debounce_delay_s

        # Active LOW: False when button is pressed, True when released
        self.is_pressed: bool = not self.io.value
        self._last_raw_value: bool = self.io.value
        self._last_change_time: float = time.monotonic()

    def update(self) -> bool:
        """
        Polls the button state with time-based debouncing.
        Returns True if the debounced pressed state changed, False otherwise.
        """
        raw_val: bool = self.io.value
        now: float = time.monotonic()

        if raw_val != self._last_raw_value:
            self._last_raw_value = raw_val
            self._last_change_time = now

        if (now - self._last_change_time) >= self.debounce_delay_s:
            debounced_pressed: bool = not raw_val
            if debounced_pressed != self.is_pressed:
                self.is_pressed = debounced_pressed
                return True

        return False


def connect_wifi(led: digitalio.DigitalInOut | None) -> bool:
    """
    Ensures that the Wi-Fi connection is active.
    Flashes the LED while connecting, then sets it solid once connected.
    """
    if wifi.radio.connected:
        return True

    print(f"Connecting to WiFi: '{WIFI_SSID}'...")
    try:
        # Blink LED while trying to connect
        for _ in range(3):
            if led is not None:
                led.value = True
                time.sleep(0.1)
                led.value = False
                time.sleep(0.1)

        wifi.radio.connect(WIFI_SSID, WIFI_PASSWORD)
        print(f"Successfully connected to WiFi! IP address: {wifi.radio.ipv4_address}")
        
        # Turn LED on solid to indicate connection success
        if led is not None:
            led.value = True
        return True
    except Exception as e:
        print(f"Failed to connect to WiFi: {e}")
        if led is not None:
            led.value = False
        return False


def trigger_ifttt(requests_session: adafruit_requests.Session, led: digitalio.DigitalInOut | None) -> None:
    """Sends the HTTP GET request to the IFTTT webhook."""
    print("Triggering IFTTT webhook...")
    
    # Briefly turn off LED to show activity start
    if led is not None:
        led.value = False

    start_time: float = time.monotonic()
    try:
        response = requests_session.get(IFTTT_URL)
        duration: float = time.monotonic() - start_time
        print(f"IFTTT Response Code: {response.status_code} (took {duration:.2f}s)")
        print(f"IFTTT Response Body: {response.text}")
        response.close()
        
        # Rapid double flash indicating HTTP request success
        if led is not None:
            for _ in range(2):
                led.value = True
                time.sleep(0.08)
                led.value = False
                time.sleep(0.08)
            led.value = True  # Back to solid connected state
    except Exception as e:
        print(f"Error sending request: {e}")
        # Slow blink indicating HTTP failure
        if led is not None:
            for _ in range(5):
                led.value = True
                time.sleep(0.3)
                led.value = False
                time.sleep(0.3)
            led.value = wifi.radio.connected  # Reset LED status based on connection


def main() -> None:
    """Main execution loop."""
    # Setup onboard LED
    led: digitalio.DigitalInOut | None = None
    try:
        led = digitalio.DigitalInOut(PIN_LED)
        led.direction = digitalio.Direction.OUTPUT
        led.value = False
    except Exception:
        # Fallback if PIN_LED is not defined on this target
        pass

    # Initialize button
    button: DebouncedButton = DebouncedButton(PIN_BUTTON)

    # Establish initial Wi-Fi connection
    connect_wifi(led)

    # Setup the HTTP session (reused across requests)
    pool: socketpool.SocketPool = socketpool.SocketPool(wifi.radio)
    ssl_context: ssl.SSLContext = ssl.create_default_context()
    requests_session: adafruit_requests.Session = adafruit_requests.Session(pool, ssl_context)

    print("System ready. Press GP14 button to trigger IFTTT webhook.")

    last_wifi_attempt: float = 0.0
    wifi_retry_cooldown: float = 30.0  # seconds

    while True:
        now: float = time.monotonic()
        # Keep WiFi alive (with a retry cooldown to keep button responsive)
        if not wifi.radio.connected and (now - last_wifi_attempt >= wifi_retry_cooldown):
            last_wifi_attempt = now
            connect_wifi(led)

        # Poll the button
        if button.update():
            if button.is_pressed:
                print("Button pressed on GP14.")
                if wifi.radio.connected or connect_wifi(led):
                    trigger_ifttt(requests_session, led)
                else:
                    print("Cannot trigger IFTTT: WiFi connection is offline.")
                last_wifi_attempt = time.monotonic()

        time.sleep(0.01)


if __name__ == "__main__":
    main()
