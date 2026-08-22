// tests/settings_portal.spec.js
// Regression test suite for settings.html Web Serial interface and UI state transitions.

const { test, expect } = require('@playwright/test');
const path = require('path');

const SETTINGS_HTML_PATH = path.resolve(__dirname, '..', 'settings.html');

test.describe('Pico Web Serial Settings Portal', () => {
  test.beforeEach(async ({ page }) => {
    // Inject Mock Web Serial API into the page before scripts load
    await page.addInitScript(() => {
      class MockSerialPort {
        constructor() {
          this.writtenLines = [];
          this.isOpen = false;
          this.openOptions = null;
          this._controller = null;
          this.shouldFailOpen = false;
          this.shouldFailWrite = false;
          this._initStreams();
        }

        _initStreams() {
          this.readable = new ReadableStream({
            start: (controller) => {
              this._controller = controller;
            },
            cancel: () => {}
          });

          this.writable = new WritableStream({
            write: (chunk) => {
              if (this.shouldFailWrite) {
                throw new Error('Mock write failure / broken pipe');
              }
              const text = new TextDecoder().decode(chunk);
              this.writtenLines.push(text);
              if (window.__onSerialWrite) {
                window.__onSerialWrite(text);
              }
            }
          });
        }

        async open(options) {
          if (this.shouldFailOpen) {
            throw new Error('Port already open / device locked');
          }
          this.isOpen = true;
          this.openOptions = options;
          return Promise.resolve();
        }

        async close() {
          this.isOpen = false;
          return Promise.resolve();
        }

        // Push data to the browser reader stream (string or JSON object)
        simulateReceive(data) {
          const text = typeof data === 'string' ? data : JSON.stringify(data) + '\n';
          const encoder = new TextEncoder();
          if (this._controller) {
            this._controller.enqueue(encoder.encode(text));
          }
        }

        simulateStreamError(errMessage) {
          if (this._controller) {
            this._controller.error(new Error(errMessage || 'Hardware stream disconnected'));
          }
        }
      }

      const mockPort = new MockSerialPort();
      window.__mockSerialPort = mockPort;

      const listeners = new Map();
      const mockSerial = {
        shouldFailRequestPort: false,
        requestPort: async () => {
          if (mockSerial.shouldFailRequestPort) {
            throw new Error('No port selected by the user.');
          }
          return mockPort;
        },
        addEventListener: (event, fn) => {
          if (!listeners.has(event)) listeners.set(event, []);
          listeners.get(event).push(fn);
        },
        removeEventListener: (event, fn) => {
          const list = listeners.get(event) || [];
          const idx = list.indexOf(fn);
          if (idx >= 0) list.splice(idx, 1);
        },
        simulateDisconnect: (targetPort = mockPort) => {
          const list = listeners.get('disconnect') || [];
          for (const fn of list) {
            fn({ type: 'disconnect', target: targetPort });
          }
        }
      };
      window.__mockSerial = mockSerial;

      Object.defineProperty(navigator, 'serial', {
        get: () => mockSerial,
        configurable: true
      });
    });

    await page.goto(`file://${SETTINGS_HTML_PATH}`);
  });

  test('initial state: UI indicates disconnected and config panel is disabled', async ({ page }) => {
    await expect(page.locator('#status-text')).toHaveText('Disconnected');
    await expect(page.locator('#status-dot')).not.toHaveClass(/connected/);
    await expect(page.locator('#btn-connect')).toHaveText('Connect Device');
    await expect(page.locator('#config-panel')).not.toHaveClass(/enabled/);
    await expect(page.locator('#alert-container')).toBeHidden();
  });

  test('connect flow: opens serial port with baudRate 115200 and sends get_state', async ({ page }) => {
    await page.click('#btn-connect');

    // Verify UI state after connection
    await expect(page.locator('#status-text')).toHaveText('Connected');
    await expect(page.locator('#status-dot')).toHaveClass(/connected/);
    await expect(page.locator('#btn-connect')).toHaveText('Disconnect');
    await expect(page.locator('#config-panel')).toHaveClass(/enabled/);

    // Verify port parameters and sent handshake command
    await expect.poll(async () => {
      return page.evaluate(() => window.__mockSerialPort?.writtenLines?.length);
    }).toBeGreaterThan(0);

    const { openOptions, lastSent } = await page.evaluate(() => {
      return {
        openOptions: window.__mockSerialPort.openOptions,
        lastSent: window.__mockSerialPort.writtenLines[0]
      };
    });

    expect(openOptions).toEqual({ baudRate: 115200 });
    expect(JSON.parse(lastSent)).toEqual({ command: 'get_state' });
  });

  test('connect failure: handles user cancelling port selection gracefully', async ({ page }) => {
    await page.evaluate(() => {
      window.__mockSerial.shouldFailRequestPort = true;
    });

    await page.click('#btn-connect');

    await expect(page.locator('#status-text')).toHaveText('Disconnected');
    await expect(page.locator('#config-panel')).not.toHaveClass(/enabled/);
    await expect(page.locator('#log-area')).toContainText('Error connecting: No port selected by the user.');
  });

  test('connect failure: handles port.open rejection (e.g. port locked)', async ({ page }) => {
    await page.evaluate(() => {
      window.__mockSerialPort.shouldFailOpen = true;
    });

    await page.click('#btn-connect');

    await expect(page.locator('#status-text')).toHaveText('Disconnected');
    await expect(page.locator('#config-panel')).not.toHaveClass(/enabled/);
    await expect(page.locator('#log-area')).toContainText('Error connecting: Port already open / device locked');
  });

  test('sync state: populates form fields and enables saving in read-write mode', async ({ page }) => {
    await page.click('#btn-connect');

    // Simulate Pico response for get_state (read-write mode active)
    await page.evaluate(() => {
      window.__mockSerialPort.simulateReceive({
        status: 'ok',
        readonly: false,
        settings: {
          device_name: 'Macro Keyboard v2',
          blink_rate: 1.5,
          feature_enabled: false
        }
      });
    });

    // Form inputs should reflect received settings
    await expect(page.locator('#device_name')).toHaveValue('Macro Keyboard v2');
    await expect(page.locator('#blink_rate')).toHaveValue('1.5');
    await expect(page.locator('#blink_rate_val')).toHaveText('1.50s');
    await expect(page.locator('#feature_enabled')).not.toBeChecked();

    // Alert container shows write-ready message and save button is enabled
    await expect(page.locator('#alert-container')).toBeVisible();
    await expect(page.locator('#alert-box')).toHaveClass(/alert-success/);
    await expect(page.locator('#alert-box')).toContainText('Ready for Writes');
    await expect(page.locator('#btn-save')).toBeEnabled();
  });

  test('sync state: handles partial settings objects safely', async ({ page }) => {
    await page.click('#btn-connect');

    // Send payload missing device_name
    await page.evaluate(() => {
      window.__mockSerialPort.simulateReceive({
        status: 'ok',
        readonly: false,
        settings: {
          blink_rate: 0.8
        }
      });
    });

    await expect(page.locator('#device_name')).toHaveValue('');
    await expect(page.locator('#blink_rate')).toHaveValue('0.8');
    await expect(page.locator('#blink_rate_val')).toHaveText('0.80s');
    await expect(page.locator('#feature_enabled')).toBeChecked();
  });

  test('read-only mode: shows warning banner and disables save button', async ({ page }) => {
    await page.click('#btn-connect');

    // Simulate Pico response when storage is read-only for CircuitPython
    await page.evaluate(() => {
      window.__mockSerialPort.simulateReceive({
        status: 'ok',
        readonly: true,
        settings: {
          device_name: 'Pico Device',
          blink_rate: 0.5,
          feature_enabled: true
        }
      });
    });

    await expect(page.locator('#alert-container')).toBeVisible();
    await expect(page.locator('#alert-box')).toHaveClass(/alert-warning/);
    await expect(page.locator('#alert-box')).toContainText('Read-Only Mode Active');
    await expect(page.locator('#btn-save')).toBeDisabled();
  });

  test('slider input: dynamically updates the blink rate text label', async ({ page }) => {
    const slider = page.locator('#blink_rate');
    await slider.fill('0.25');
    await slider.dispatchEvent('input');

    await expect(page.locator('#blink_rate_val')).toHaveText('0.25s');

    await slider.fill('1.75');
    await slider.dispatchEvent('input');
    await expect(page.locator('#blink_rate_val')).toHaveText('1.75s');
  });

  test('save configuration: sends set_settings command with form values over serial', async ({ page }) => {
    await page.click('#btn-connect');

    // Put into read-write mode so save is enabled
    await page.evaluate(() => {
      window.__mockSerialPort.simulateReceive({
        status: 'ok',
        readonly: false,
        settings: {
          device_name: 'Old Name',
          blink_rate: 0.5,
          feature_enabled: false
        }
      });
    });

    // Update form fields
    await page.locator('#device_name').fill('Custom Desk Light');
    await page.locator('#blink_rate').fill('0.75');
    await page.locator('#blink_rate').dispatchEvent('input');
    await page.locator('.switch').click();

    // Submit configuration
    await page.click('#btn-save');

    // Verify sent JSON payload
    await expect.poll(async () => {
      return page.evaluate(() => window.__mockSerialPort?.writtenLines?.length);
    }).toBeGreaterThanOrEqual(2);

    const lastSent = await page.evaluate(() => {
      const lines = window.__mockSerialPort.writtenLines;
      return lines[lines.length - 1];
    });

    expect(JSON.parse(lastSent)).toEqual({
      command: 'set_settings',
      settings: {
        device_name: 'Custom Desk Light',
        blink_rate: 0.75,
        feature_enabled: true
      }
    });
  });

  test('save failure: handles serial write error without unhandled rejection', async ({ page }) => {
    await page.click('#btn-connect');

    await page.evaluate(() => {
      window.__mockSerialPort.shouldFailWrite = true;
      window.__mockSerialPort.simulateReceive({
        status: 'ok',
        readonly: false,
        settings: { device_name: 'Pico', blink_rate: 0.5, feature_enabled: true }
      });
    });

    await page.click('#btn-save');

    await expect(page.locator('#log-area')).toContainText('Error sending data: Mock write failure / broken pipe');
  });

  test('stream buffering: correctly parses fragmented serial chunks across newlines', async ({ page }) => {
    await page.click('#btn-connect');

    // Send JSON in multiple fragmented chunks
    await page.evaluate(() => {
      const part1 = '{"status":"ok","settings":{"device_name":"Frag';
      const part2 = 'mented Device","blink_rate":0.2,"feature_enabled":true},"readonly":false}\n';
      window.__mockSerialPort.simulateReceive(part1);
      setTimeout(() => {
        window.__mockSerialPort.simulateReceive(part2);
      }, 50);
    });

    await expect(page.locator('#device_name')).toHaveValue('Fragmented Device');
    await expect(page.locator('#blink_rate_val')).toHaveText('0.20s');
    await expect(page.locator('#feature_enabled')).toBeChecked();
  });

  test('stream parsing: processes multiple JSON lines received in a single chunk', async ({ page }) => {
    await page.click('#btn-connect');

    await page.evaluate(() => {
      const msg1 = JSON.stringify({ status: 'ok', settings: { device_name: 'First Name', blink_rate: 0.4 } });
      const msg2 = JSON.stringify({ status: 'ok', settings: { device_name: 'Second Name', blink_rate: 1.2 } });
      window.__mockSerialPort.simulateReceive(`${msg1}\n${msg2}\n`);
    });

    // The second message in the batch should be applied last
    await expect(page.locator('#device_name')).toHaveValue('Second Name');
    await expect(page.locator('#blink_rate')).toHaveValue('1.2');
    await expect(page.locator('#blink_rate_val')).toHaveText('1.20s');
  });

  test('resilience: ignores non-JSON serial logs/noise and parses subsequent valid JSON', async ({ page }) => {
    await page.click('#btn-connect');

    await page.evaluate(() => {
      // Noise like CircuitPython boot logs or print statements
      const noise = 'Auto-reload is on. Simply save files over USB to run them.\n';
      const valid = JSON.stringify({
        status: 'ok',
        readonly: false,
        settings: { device_name: 'Recovered Device', blink_rate: 0.9, feature_enabled: true }
      }) + '\n';
      window.__mockSerialPort.simulateReceive(noise + valid);
    });

    await expect(page.locator('#log-area')).toContainText('Failed to parse response: Auto-reload is on.');
    await expect(page.locator('#device_name')).toHaveValue('Recovered Device');
    await expect(page.locator('#blink_rate_val')).toHaveText('0.90s');
  });

  test('stream error: handles stream read failure during active session cleanly', async ({ page }) => {
    await page.click('#btn-connect');
    await expect(page.locator('#status-text')).toHaveText('Connected');

    // Trigger error on active stream
    await page.evaluate(() => {
      window.__mockSerialPort.simulateStreamError('Sudden USB power loss');
    });

    await expect(page.locator('#status-text')).toHaveText('Disconnected');
    await expect(page.locator('#config-panel')).not.toHaveClass(/enabled/);
    await expect(page.locator('#log-area')).toContainText('Read error (likely disconnected): Sudden USB power loss');
  });

  test('disconnect button: resets UI back to disconnected state and clears storage locks', async ({ page }) => {
    await page.click('#btn-connect');
    await expect(page.locator('#status-text')).toHaveText('Connected');

    // Set to read-only initially so Save button is disabled
    await page.evaluate(() => {
      window.__mockSerialPort.simulateReceive({ status: 'ok', readonly: true });
    });
    await expect(page.locator('#btn-save')).toBeDisabled();

    // Click Disconnect
    await page.click('#btn-connect');

    await expect(page.locator('#status-text')).toHaveText('Disconnected');
    await expect(page.locator('#status-dot')).not.toHaveClass(/connected/);
    await expect(page.locator('#btn-connect')).toHaveText('Connect Device');
    await expect(page.locator('#config-panel')).not.toHaveClass(/enabled/);
    await expect(page.locator('#alert-container')).toBeHidden();
    await expect(page.locator('#btn-save')).toBeEnabled(); // Reset state
  });

  test('physical USB disconnect: listener triggers cleanup when device is unplugged', async ({ page }) => {
    await page.click('#btn-connect');
    await expect(page.locator('#status-text')).toHaveText('Connected');

    // Simulate browser-level USB unplug event
    await page.evaluate(() => {
      navigator.serial.simulateDisconnect();
    });

    await expect(page.locator('#status-text')).toHaveText('Disconnected');
    await expect(page.locator('#config-panel')).not.toHaveClass(/enabled/);
  });

  test('physical USB disconnect: ignores disconnect events from other serial devices', async ({ page }) => {
    await page.click('#btn-connect');
    await expect(page.locator('#status-text')).toHaveText('Connected');

    // Simulate disconnect of an unrelated other port
    await page.evaluate(() => {
      const unrelatedPort = {};
      navigator.serial.simulateDisconnect(unrelatedPort);
    });

    // Should remain connected
    await expect(page.locator('#status-text')).toHaveText('Connected');
    await expect(page.locator('#config-panel')).toHaveClass(/enabled/);
  });

  test('disconnected form submission: submitting form while disconnected does nothing safely', async ({ page }) => {
    // Attempt form submission when port is null
    const submitResult = await page.evaluate(() => {
      const form = document.getElementById('settings-form');
      const submitEvent = new Event('submit', { cancelable: true });
      form.dispatchEvent(submitEvent);
      return window.__mockSerialPort?.writtenLines?.length || 0;
    });

    expect(submitResult).toBe(0);
    await expect(page.locator('#status-text')).toHaveText('Disconnected');
  });

  test('error handling: logs error without crashing when board returns error response', async ({ page }) => {
    // Handle window alert dialog
    let dialogMessage = '';
    page.on('dialog', async (dialog) => {
      dialogMessage = dialog.message();
      await dialog.accept();
    });

    await page.click('#btn-connect');

    // Simulate error response from Pico
    await page.evaluate(() => {
      window.__mockSerialPort.simulateReceive({
        status: 'error',
        message: 'Storage full or filesystem write locked'
      });
    });

    await expect.poll(() => dialogMessage).toBe('Storage full or filesystem write locked');
    await expect(page.locator('#log-area')).toContainText('Board reported error: Storage full or filesystem write locked');
  });
});
