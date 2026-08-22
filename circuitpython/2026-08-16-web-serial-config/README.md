# Pico USB Web Serial Configuration Portal

A real-time configuration portal for Raspberry Pi Pico (CircuitPython) using the browser's **Web Serial API** over a secondary USB CDC data channel.

The browser UI ([`settings.html`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/settings.html)) communicates with the Pico using newline-delimited JSON commands to read device status and persist settings (device name, LED blink rate, status blink toggle) to `settings.json`.

---

## Hardware & Pin Configuration

| Pico Pin | Component / Function | Description |
| :--- | :--- | :--- |
| **GP14** (Pin 19) | Storage Mode Switch / Jumper | Controls whether the Pico or Host PC has write access to the filesystem |
| **GND** (Pin 18 or 23) | Ground | Connected to GP14 to enable Host PC write access |
| **Onboard LED** | Status Indicator | Blinks according to configured `blink_rate` and `feature_enabled` settings |

---

## Storage Access Modes (GP14)

CircuitPython does not allow concurrent write access to the flash filesystem from both the Pico and the Host PC at the same time. [`boot.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/boot.py) checks the state of **GP14** during boot:

* **Pico Read-Write Mode (GP14 Disconnected / Open / HIGH):**
  * The filesystem is writable by CircuitPython and read-only to the Host PC.
  * The web portal can save configuration changes to `settings.json`.
* **Host PC Read-Write Mode (GP14 Grounded / LOW):**
  * The filesystem is writable by the Host PC and read-only to CircuitPython.
  * The web portal operates in read-only mode (Save button is disabled).

---

## How to Try It

### 1. Copy Files to the Pico
1. Ensure GP14 is connected to GND (or default host write mode is active).
2. Copy the project files to your `CIRCUITPY` drive:
   ```bash
   cp boot.py code.py /run/media/$USER/CIRCUITPY/
   ```
3. Disconnect GP14 from GND (leave it open/floating) so that CircuitPython can write to storage.
4. Hard reset or power cycle the Pico (unplug and re-plug USB) so [`boot.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/boot.py) re-enumerates the USB CDC ports and remounts storage as Pico-writable.

### 2. Open the Web Portal
1. Open [`settings.html`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/settings.html) in a Web Serial-supported browser (Google Chrome, Microsoft Edge, or Chromium).
2. Click **Connect Device**.
3. In the browser permission prompt, select the serial port for **Pico Config Portal** (the secondary CDC data port).
4. The dashboard will connect, display real-time status, and populate current settings.
5. Modify settings (e.g. adjust the LED blink rate slider or change device name) and click **Save Configuration** to update `settings.json` on the Pico.

---

## How to Reset the Pico to Host Read-Write Mode

When the filesystem is mounted as writable by CircuitPython, your host computer cannot edit or save files on the `CIRCUITPY` drive. To switch back:

1. **Connect GP14 to GND** using a jumper wire (GP14 is Pin 19; GND is Pin 18 or 23).
2. **Reboot the Pico** by either:
   * Unplugging and plugging the USB cable back in, or
   * Momentarily shorting **RUN** (Pin 30) to **GND** (Pin 28).
3. Upon booting, [`boot.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/boot.py) detects GP14 is pulled LOW and remounts the filesystem as read-only for CircuitPython and **read-write for the Host PC**.
4. You can now edit [`code.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/code.py), [`boot.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/boot.py), or other files directly from your computer.
