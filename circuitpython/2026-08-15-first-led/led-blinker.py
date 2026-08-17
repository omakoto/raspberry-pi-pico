#!/usr/bin/env circuit-run
import time
import board
import digitalio

# Pin Definitions
PIN_LED: board.Pin = board.LED

# Setup the onboard LED
led = digitalio.DigitalInOut(PIN_LED)
led.direction = digitalio.Direction.OUTPUT

while True:
    led.value = True
    time.sleep(0.5)
    led.value = False
    time.sleep(0.5)
    print("OK")
