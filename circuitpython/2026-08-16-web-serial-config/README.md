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

## Storage Access Modes (GP14 / Pin 19)

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
   cp boot.py code.py settings.html /run/media/$USER/CIRCUITPY/
   ```
3. Disconnect GP14 from GND (leave it open/floating) so that CircuitPython can write to storage.
4. Hard reset or power cycle the Pico (unplug and re-plug USB) so [`boot.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/boot.py) re-enumerates the USB CDC ports and remounts storage as Pico-writable.

### 2. Open the Web Portal
1. Open the settings portal in Chrome directly from the connected device using [`open-settings.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/open-settings.sh):
   ```bash
   ./open-settings.sh
   ```
   *(Or manually open the `settings.html` file from the mounted `CIRCUITPY` drive, e.g. `/run/media/$USER/CIRCUITPY/settings.html`, in Google Chrome, Microsoft Edge, or any Web Serial-compatible browser).*
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

---

## How to Test

The web configuration dashboard includes an automated browser regression test suite in [`tests/settings_portal.spec.js`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config/tests/settings_portal.spec.js) built with **Playwright**. It runs against headless Chromium using a mock implementation of the browser's Web Serial API (`navigator.serial`).

### 1. Prerequisites & Setup

Install project dependencies and the Chromium browser binary:

```bash
npm install
npx playwright install chromium
```

### 2. Run Tests

```bash
# Run all regression tests in headless mode
npm test

# Run tests with interactive UI / trace inspector
npx playwright test --ui

# Run tests in headed browser mode
npx playwright test --headed
```

### 3. Test Coverage

The test suite validates:
* **Connection Lifecycle**: Port opening at `115200` baud, handshake query (`{"command":"get_state"}`), UI status transitions, and reader stream management.
* **Storage Mode Detection**: Proper rendering of write-ready and read-only status alerts, plus locking the Save button when the Pico's storage is write-protected.
* **State Synchronization & Live Controls**: Real-time slider text updates (`0.50s`), custom toggle state sync, and form population from incoming JSON.
* **Configuration Persistence**: Verification of the `set_settings` JSON payload sent through the serial stream on form submission.
* **Stream Fragmentation & Chunking**: Line buffer stability when serial data arrives in fragmented chunks across newlines.
* **Disconnection & Hardware Events**: Clean resource release and UI reset on manual disconnect or physical USB unplug (`navigator.serial` `'disconnect'` event).
* **Error Resilience**: Graceful error handling and debug logging when the board returns error payloads.

