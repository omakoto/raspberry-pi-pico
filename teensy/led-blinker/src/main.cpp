//
// Simple LED blinker and USB Serial heartbeat for Teensy 4.0 / 4.1
//
// Uses the onboard LED on Pin 13 (LED_BUILTIN) and prints tick messages
// over USB CDC serial so that serial monitor tools (02-monitor.sh) can verify
// board responsiveness.
//

#include <Arduino.h>

void setup() {
    // Initialize USB CDC serial output
    Serial.begin(115200);

    // Configure onboard LED pin as output
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    static uint32_t counter = 0;

    digitalWrite(LED_BUILTIN, HIGH);
    Serial.printf("[Teensy 4.x] LED ON  | Cycle: %lu | Uptime: %lu ms\r\n", ++counter, millis());
    delay(500);

    digitalWrite(LED_BUILTIN, LOW);
    Serial.printf("[Teensy 4.x] LED OFF | Cycle: %lu | Uptime: %lu ms\r\n", counter, millis());
    delay(500);
}
