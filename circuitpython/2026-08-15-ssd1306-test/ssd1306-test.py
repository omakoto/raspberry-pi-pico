#!/usr/bin/env circuit-run
"""
Script to control and test an SSD1306 OLED display (128x64) on a Raspberry Pi Pico.

Pin Connections:
- GP2: I2C1 SDA (Physical Pin 4)
- GP3: I2C1 SCL (Physical Pin 5)
- VCC: 3.3V
- GND: GND
"""

import board
import digitalio
import busio
import time

# 3x5 Font definitions (column-major order) for rendering characters on the screen
FONT: dict[str, list[int]] = {
    ' ': [0x00, 0x00, 0x00],
    'A': [0x1E, 0x05, 0x1E],
    'B': [0x1F, 0x15, 0x0A],
    'C': [0x0E, 0x11, 0x11],
    'D': [0x1F, 0x11, 0x0E],
    'E': [0x1F, 0x15, 0x11],
    'F': [0x1F, 0x05, 0x01],
    'G': [0x0E, 0x15, 0x1D],
    'H': [0x1F, 0x04, 0x1F],
    'I': [0x11, 0x1F, 0x11],
    'J': [0x10, 0x10, 0x0F],
    'K': [0x1F, 0x04, 0x1B],
    'L': [0x1F, 0x10, 0x10],
    'M': [0x1F, 0x02, 0x0C, 0x02, 0x1F],
    'N': [0x1F, 0x02, 0x04, 0x08, 0x1F],
    'O': [0x0E, 0x11, 0x0E],
    'P': [0x1F, 0x09, 0x06],
    'Q': [0x0E, 0x11, 0x1E],
    'R': [0x1F, 0x09, 0x16],
    'S': [0x12, 0x15, 0x09],
    'T': [0x01, 0x1F, 0x01],
    'U': [0x0F, 0x10, 0x0F],
    'V': [0x07, 0x18, 0x07],
    'W': [0x1F, 0x10, 0x0E, 0x10, 0x1F],
    'X': [0x1B, 0x04, 0x1B],
    'Y': [0x03, 0x1C, 0x03],
    'Z': [0x19, 0x15, 0x13],
    '0': [0x0E, 0x11, 0x0E],
    '1': [0x12, 0x1F, 0x10],
    '2': [0x12, 0x15, 0x19],
    '3': [0x11, 0x15, 0x1F],
    '4': [0x07, 0x04, 0x1F],
    '5': [0x17, 0x15, 0x1D],
    '6': [0x1E, 0x15, 0x1D],
    '7': [0x11, 0x09, 0x07],
    '8': [0x1A, 0x15, 0x1A],
    '9': [0x13, 0x15, 0x1E],
    '-': [0x04, 0x04, 0x04],
    ':': [0x0A, 0x00, 0x00],
    '.': [0x10, 0x00, 0x00],
    '!': [0x00, 0x1D, 0x00]
}

class SSD1306:
    """A lightweight driver for the SSD1306 OLED display using pure busio.I2C."""
    def __init__(self, i2c: busio.I2C, addr: int = 0x3C, width: int = 128, height: int = 64) -> None:
        self.i2c: busio.I2C = i2c
        self.addr: int = addr
        self.width: int = width
        self.height: int = height
        self.buffer: bytearray = bytearray((width * height) // 8)
        
        # Standard 128x64 display initialization commands
        init_cmds: list[int] = [
            0xAE,        # Display OFF
            0xD5, 0x80,  # Set display clock divide ratio/oscillator frequency
            0xA8, 0x3F,  # Set multiplex ratio (1 to 64)
            0xD3, 0x00,  # Set display offset to 0
            0x40 | 0x00, # Set display start line to 0
            0x8D, 0x14,  # Enable charge pump
            0x20, 0x00,  # Set memory addressing mode to Horizontal
            0xA1,        # Set segment re-map (COL127 mapped to SEG0)
            0xC8,        # Set COM Output Scan Direction (remap)
            0xDA, 0x12,  # Set COM pins hardware configuration
            0x81, 0xCF,  # Set contrast control to 0xCF
            0xD9, 0xF1,  # Set pre-charge period
            0xDB, 0x40,  # Set VCOMH deselect level
            0xA4,        # Entire display ON (resume to RAM content)
            0xA6,        # Set normal display (not inverse)
            0xAF         # Display ON
        ]
        self.write_cmd(init_cmds)
        self.clear()
        self.show()

    def write_cmd(self, cmds: list[int]) -> None:
        """Sends a list of commands to the display controller."""
        self.i2c.writeto(self.addr, bytes([0x00] + cmds))

    def clear(self, color: bool = False) -> None:
        """Clears the local framebuffer buffer."""
        fill_val: int = 0xFF if color else 0x00
        for i in range(len(self.buffer)):
            self.buffer[i] = fill_val

    def pixel(self, x: int, y: int, color: bool) -> None:
        """Draws a single pixel in the framebuffer."""
        if x < 0 or x >= self.width or y < 0 or y >= self.height:
            return
        page: int = y // 8
        bit: int = y % 8
        index: int = (page * self.width) + x
        if color:
            self.buffer[index] |= (1 << bit)
        else:
            self.buffer[index] &= ~(1 << bit)

    def line(self, x0: int, y0: int, x1: int, y1: int, color: bool) -> None:
        """Draws a line from (x0, y0) to (x1, y1) using Bresenham's algorithm."""
        dx: int = abs(x1 - x0)
        dy: int = abs(y1 - y0)
        sx: int = 1 if x0 < x1 else -1
        sy: int = 1 if y0 < y1 else -1
        err: int = dx - dy
        while True:
            self.pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2: int = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def rect(self, x: int, y: int, w: int, h: int, color: bool) -> None:
        """Draws an unfilled rectangle outline."""
        self.line(x, y, x + w - 1, y, color)
        self.line(x, y + h - 1, x + w - 1, y + h - 1, color)
        self.line(x, y, x, y + h - 1, color)
        self.line(x + w - 1, y, x + w - 1, y + h - 1, color)

    def char(self, c: str, x: int, y: int, color: bool) -> int:
        """Renders a single font character and returns its drawn width."""
        c = c.upper()
        if c not in FONT:
            return 0
        pattern: list[int] = FONT[c]
        for col_idx, col_data in enumerate(pattern):
            for row_idx in range(8):
                if col_data & (1 << row_idx):
                    self.pixel(x + col_idx, y + row_idx, color)
        return len(pattern) + 1  # Returns width of character plus 1 pixel gap

    def text(self, s: str, x: int, y: int, color: bool) -> None:
        """Renders a string of text onto the screen buffer."""
        curr_x: int = x
        for c in s:
            width: int = self.char(c, curr_x, y, color)
            curr_x += width

    def show(self) -> None:
        """Pushes the local framebuffer to the physical OLED display."""
        # Set bounds for display updates
        self.write_cmd([0x21, 0x00, 127]) # Columns 0 to 127
        self.write_cmd([0x22, 0x00, 7])   # Pages 0 to 7 (8 pixels high each)
        
        # Write buffer data in chunks of 128 bytes to stay within device limits
        for i in range(0, len(self.buffer), 128):
            chunk: bytearray = self.buffer[i:i+128]
            self.i2c.writeto(self.addr, bytes([0x40]) + chunk)


# Initialize the onboard LED for diagnostics
led: digitalio.DigitalInOut = digitalio.DigitalInOut(board.LED)
led.direction = digitalio.Direction.OUTPUT

# Blink onboard LED to signal script start
for _ in range(5):
    led.value = True
    time.sleep(0.1)
    led.value = False
    time.sleep(0.1)

print("SSD1306 Display Test Initialized.")

i2c = None
while i2c is None:
    try:
        # Initialize I2C1 bus (SCL=GP3, SDA=GP2)
        i2c = busio.I2C(board.GP3, board.GP2)
        print("I2C1 bus successfully initialized.")
    except Exception as e:
        print(f"I2C1 Initialization Error (Check SSD1306 VCC/GND power and SDA/SCL wiring): {e}")
        time.sleep(2.0)

oled = None
while oled is None:
    if i2c.try_lock():
        try:
            addresses: list[int] = i2c.scan()
            if 0x3C in addresses:
                oled = SSD1306(i2c, addr=0x3C)
                print("SSD1306 OLED display detected and initialized.")
            else:
                print(f"OLED not detected. Found I2C addresses: {[hex(a) for a in addresses]}")
                time.sleep(2.0)
        except Exception as e:
            print(f"OLED Detection/Initialization Error: {e}")
            time.sleep(2.0)
        finally:
            i2c.unlock()

# Simple animation test loop
box_x: int = 10
box_y: int = 25
box_dx: int = 2
box_dy: int = 1
box_w: int = 8
box_h: int = 8

while True:
    # Toggle diagnostic LED
    led.value = True
    
    if i2c.try_lock():
        try:
            # Clear frame
            oled.clear()

            # Draw static screen layout
            oled.rect(0, 0, 128, 64, True)
            oled.text("PICO 2 W", 35, 5, True)
            oled.text("SSD1306 OLED TEST", 18, 15, True)
            oled.text("STATUS: OK", 38, 54, True)

            # Move and draw bouncing box
            box_x += box_dx
            box_y += box_dy

            # Bounce off horizontal boundaries
            if box_x <= 1 or box_x + box_w >= 127:
                box_dx = -box_dx
            # Bounce off vertical text boundaries
            if box_y <= 23 or box_y + box_h >= 53:
                box_dy = -box_dy

            oled.rect(box_x, box_y, box_w, box_h, True)
            oled.show()
        except Exception as e:
            print(f"Display Error: {e}")
        finally:
            i2c.unlock()

    led.value = False
    time.sleep(0.02)
