#ifndef STATUS_LED_H_
#define STATUS_LED_H_

// The board's own LED, used as the pairing indicator: the XIAO's yellow user LED (plain GPIO) or the
// DevKitC's WS2812 RGB LED. UI task only.
void status_led_init();
void status_led_set(bool on);

#endif // STATUS_LED_H_
