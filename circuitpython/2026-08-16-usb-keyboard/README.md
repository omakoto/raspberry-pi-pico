# Raspberry Pi Pico USB Keyboard ('a' and 'b' keys)

A CircuitPython project that configures a Raspberry Pi Pico (or Pico 2 / Pico 2 W) as a USB HID Keyboard. 

Two push buttons act as the **'a'** and **'b'** keys.

## Hardware Wiring

| Pico Pin | Component | Description |
| :--- | :--- | :--- |
| **GP17** (Pin 22) | Push Button 1 | 'a' key (active LOW, internal pull-up) |
| **GP18** (Pin 24) | Push Button 2 | 'b' key (active LOW, internal pull-up) |
| **GND** (e.g. Pin 23 or Pin 18) | Button Common | Connected to the other side of both buttons |

> Note: The script utilizes internal pull-up resistors on GP17 and GP18, so no external pull-up resistors are required. Simply wire each button between its GPIO pin and GND.

## Features

- **USB HID Keyboard**: Emulates a standard USB keyboard via CircuitPython's built-in `usb_hid` module with zero external library dependencies.
- **Hold & Release Support**: Pressing and holding down a button holds the key down, supporting standard operating system key auto-repeat.
- **Software Debouncing**: Includes a non-blocking 20ms debouncing filter on button transitions to eliminate contact chatter.
- **Multi-key (Roll-over) Support**: Supports pressing both buttons simultaneously.
- **Visual Feedback**: The onboard LED lights up whenever either button is actively pressed.

## How to Run

1. Connect the Raspberry Pi Pico running CircuitPython to your computer via USB.
2. Execute the script directly:
   ```bash
   ./usb-keyboard.py
   ```
   Or copy the file to your CircuitPython drive as `code.py`:
   ```bash
   cp usb-keyboard.py /run/media/omakoto/CIRCUITPY/code.py
   ```
3. Open any text editor and press the buttons on GP17 and GP18 to type `a` and `b`.
