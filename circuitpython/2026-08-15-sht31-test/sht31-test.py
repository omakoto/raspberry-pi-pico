#!/usr/bin/env circuit-run
"""
Script to read and print temperature and humidity from an SHT31 sensor on a Raspberry Pi Pico.

Pin Connections:
- GP10: I2C1 SDA
- GP11: I2C1 SCL
- VCC: 3.3V
- GND: GND
"""

import board
import digitalio
import busio
import time

# Pin Definitions
PIN_LED: board.Pin = board.LED
PIN_I2C_SDA: board.Pin = board.GP10
PIN_I2C_SCL: board.Pin = board.GP11

def check_crc(data: bytes) -> bool:
    """
    Calculates 8-bit checksum (CRC-8) for SHT3x data block.
    Polynomial: x^8 + x^5 + x^4 + 1 (0x31)
    Initialization: 0xFF
    """
    crc: int = 0xFF
    for byte in data[:2]:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = (crc << 1) ^ 0x31
            else:
                crc <<= 1
            crc &= 0xFF
    return crc == data[2]

# Initialize the onboard LED for visual diagnostics
led: digitalio.DigitalInOut = digitalio.DigitalInOut(PIN_LED)
led.direction = digitalio.Direction.OUTPUT

# Blink rapidly 5 times to show script execution has started
for _ in range(5):
    led.value = True
    time.sleep(0.1)
    led.value = False
    time.sleep(0.1)

print("SHT31 Temperature & Humidity Sensor Test Initialized.")

i2c = None
while i2c is None:
    try:
        # Initialize the I2C bus
        i2c = busio.I2C(PIN_I2C_SCL, PIN_I2C_SDA)
        print("I2C bus successfully initialized.")
    except Exception as e:
        print(f"I2C Initialization Error (Check SHT31 VCC/GND power and SDA/SCL wiring): {e}")
        time.sleep(2.0)

SHT31_ADDR: int = 0x44

# Attempt to scan for the sensor
if i2c.try_lock():
    try:
        addresses: list[int] = i2c.scan()
        if SHT31_ADDR in addresses:
            print(f"SHT31 sensor detected at I2C address {hex(SHT31_ADDR)}.")
        else:
            print(f"Warning: SHT31 sensor not detected. Found addresses: {[hex(a) for a in addresses]}")
            print("Please check if SDA (GP10) and SCL (GP11) pins might be swapped.")
    except Exception as e:
        print(f"I2C Scan Error (check if SDA/SCL are swapped or sensor is powered): {e}")
    finally:
        i2c.unlock()

while True:
    # Toggle LED to show loop activity
    led.value = True
    time.sleep(0.05)
    led.value = False

    if i2c.try_lock():
        try:
            # Trigger high-repeatability measurement with no clock stretching (0x2400)
            i2c.writeto(SHT31_ADDR, bytes([0x24, 0x00]))
            
            # Wait for measurement to complete (SHT31 needs max 15ms)
            time.sleep(0.02)
            
            # Read 6 bytes of measurement data
            buffer: bytearray = bytearray(6)
            i2c.readfrom_into(SHT31_ADDR, buffer)
            
            # Extract temperature and humidity data blocks with CRCs
            temp_data: bytes = bytes(buffer[0:3])
            humi_data: bytes = bytes(buffer[3:6])
            
            # Validate CRCs
            if check_crc(temp_data) and check_crc(humi_data):
                # Convert raw temperature bytes to Celsius
                raw_temp: int = (temp_data[0] << 8) | temp_data[1]
                temp_c: float = -45.0 + 175.0 * (raw_temp / 65535.0)
                temp_f: float = temp_c * 1.8 + 32.0
                
                # Convert raw humidity bytes to relative humidity percentage
                raw_humi: int = (humi_data[0] << 8) | humi_data[1]
                humi_rh: float = 100.0 * (raw_humi / 65535.0)
                
                print(f"Temperature: {temp_c:.2f} °C ({temp_f:.2f} °F), Humidity: {humi_rh:.2f} %")
            else:
                print("Error: CRC validation failed.")
        except Exception as e:
            print(f"Error reading from sensor: {e}")
        finally:
            i2c.unlock()
            
    time.sleep(2.0)
