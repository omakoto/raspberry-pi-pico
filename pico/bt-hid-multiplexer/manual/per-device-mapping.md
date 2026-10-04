# Per-device mapping: remap one device without affecting the others

By default the keymap you edit in [VIAL](https://vial.rocks/) is shared by every keyboard and mouse connected to the multiplexer. If you remap the mouse wheel to volume, *every* mouse's wheel becomes volume.

**Per-device mapping** fixes that. You *bind* one Bluetooth device to one keymap layer. That device is then looked up on its own layer first, so whatever you put there applies to that device only. Everything you leave **Transparent** on that layer behaves as before.

Example used throughout this manual: a Logitech device with a vertical wheel, a horizontal wheel and four side buttons, where the vertical wheel should control the volume, while the wheels of your other mice keep scrolling.

---

**Tip:** `dl` is a short alias for `devlayer`; everywhere below, `dl 3` works the same as `devlayer 3`.

---

## What you need

- The multiplexer flashed and running (see the main [README](../README.md)).
- The device paired with the multiplexer (see [How to Pair a Device](../README.md#how-to-pair-a-device)) and working.
- VIAL access from Chrome (on Linux, run `~/cbin/setup/config-hidraw-permission` once, see [Keymapping with VIAL](../README.md#keymapping-with-vial)).
- The serial console, which is where the binding is made: run `./02-monitor.sh` from the project directory. Anything you type there is a *console command*.

---

## Step 1. Pick a layer for the device

The keymap has 4 layers (0–3). Layer 0 is the base layer shared by all devices. Choose one of layers **1, 2 or 3** for the device. Each layer can be used by several devices, but a layer you also use for `MO(n)` layer keys then changes what those keys do for the bound device, so pick one you are not using for anything else.

This manual uses **layer 3**.

## Step 2. Bind the device to the layer

The multiplexer does not make you pick the device by name or ID. It binds the device that sent input most recently:

1. Make sure **only the device you want to bind** is being used: put your hand on it and leave your other keyboards and mice alone. For the example, scroll the Logitech wheel once or click one of its buttons.
2. Immediately type this in the serial console:

   ```
   devlayer 3
   ```

3. You should see:

   ```
   Device 2 bound to layer 3. Edit that layer in VIAL.
   ```

   (The device number depends on the order devices connected in.) If you see `No device has sent input yet`, move or click the device and try again.

4. Check it:

   ```
   devlayer list
   ```

   shows every connected device with its index, name, Bluetooth address and bound layer, and the saved bindings. Your device should say `layer 3`.

If the wrong device got bound (because another mouse moved in between), fix it with `devlayer clear` (unbinds the device used last) or `devlayer clear <idx>` and start over. You can also bind explicitly by index from `devlayer list`: `devlayer <idx> <layer>`.

The binding is saved in flash by the device's Bluetooth address. It survives reconnects and reboots. Nothing changes yet, because layer 3 is still all Transparent.

## Step 3. Edit the layer in VIAL

1. Open <https://vial.rocks/> in Chrome, click **Start** and select the multiplexer.
2. Select **layer 3** with VIAL's layer selector.
3. Find the **bottom row** of the key map, the mouse row (below the F13–F24 row). From left to right it contains:
   - **8 mouse buttons**, in order: button 1 (left), 2 (right), 3 (middle), 4, 5, 6, 7, 8.
   - A gap, then **8 movement keys**: cursor up, cursor down, cursor left, cursor right, **wheel up, wheel down, wheel left, wheel right**.
4. Click the key you want to change, then pick a keycode from VIAL's keycode list (the volume keys are in its media section). For the example:
   - the **wheel up** key → **Volume Up** (`KC_VOLU`)
   - the **wheel down** key → **Volume Down** (`KC_VOLD`)

   Changes apply immediately and are saved to flash half a second after your last edit.
5. Leave everything else **Transparent** (`KC_TRNS`, which is what every key on layers 1–3 starts as): those inputs behave as on layer 0.

### Which physical input is which key

| The device does | Its key in the mouse row |
| --- | --- |
| vertical wheel up / down | wheel up / wheel down |
| horizontal wheel left / right | wheel left / wheel right |
| moves the cursor | cursor up / down / left / right |
| Linux `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE` | mouse buttons 1, 2, 3 |
| Linux `BTN_SIDE` | mouse button 4 (the usual "back" button) |
| Linux `BTN_EXTRA` | mouse button 5 (the usual "forward" button) |
| Linux `BTN_FORWARD` | mouse button 6 (default: F13) |
| Linux `BTN_BACK` | mouse button 7 (default: F14) |
| a mouse's 8th button | mouse button 8 (default: F15) |

#### Mouse buttons 6, 7 and 8

VIAL has keycodes for **mouse buttons 1–5 only**, and the multiplexer's USB mouse has 5 buttons. That has two consequences:

- A device's buttons 6–8 **cannot be sent to the PC as mouse buttons**, and you cannot assign "mouse button 6" to any key in VIAL.
- Instead, buttons 6, 7 and 8 are keys of their own in the mouse row (the 6th, 7th and 8th of the eight button keys), which you can remap to anything else: a keyboard key, a volume key or a layer key. By default they are **F13, F14 and F15**, so that they do something harmless and recognisable (no normal application uses those keys) until you decide what they should do.

Buttons 4 and 5 are the ones browsers use for back and forward, so on the example device the two *top* buttons already work as back/forward, while the two *bottom* buttons (6 and 7) send F13 and F14 until you remap them. To make them do something useful, put the keycode you want (for example `LALT(KC_LEFT)` for browser back) on those keys, on the device's layer if only this device should do it.

## Step 4. Try it

Scroll the bound device's vertical wheel: each notch taps Volume Up or Down. Scroll with another mouse: it scrolls as usual. If the bound device's other inputs also need to keep working as before, nothing more is needed; they fall through to layer 0.

---

## What can go on a key

- **Another mouse action**, for example wheel → cursor movement, or swapping axes.
- **A keyboard key**, a modifier or a modified key such as `LSFT(KC_A)`. A key on a *wheel or movement* position is **tapped once per wheel notch** (for movement, once per 24 counts of motion, see `MOUSE_COUNTS_PER_WHEEL_NOTCH` in `src/config.h`).
- **Volume keys:** `KC_MUTE`, `KC_VOLU`, `KC_VOLD`.
- **A layer key** (`MO(1)` and so on) on one of the device's buttons. While held, that layer is looked up first, then the device's layer, then layer 0. This makes "hold a button and the wheel does something else" possible for just this device.
- `KC_NO` to disable an input.

Not available: other media keys (play/pause, next, previous), macros, tap dance, mod-tap.

## Changing or removing a binding

| To do this | Type |
| --- | --- |
| Move a device to another layer | move the device, then `devlayer 2` |
| Unbind the device used last | `devlayer clear` |
| Unbind a specific device | `devlayer clear <idx>` |
| See devices and bindings | `devlayer list` |
| Reset the keymap but keep the bindings | `resetkeymap` |
| Remove all bindings (together with bonds and keymap) | `reset` |

Up to 8 devices can be bound at the same time.

## Troubleshooting

- **`No device has sent input yet`:** the multiplexer only remembers the device that sent input since it booted or since that device reconnected. Move or click the device, then run the command again.
- **It bound the wrong device:** another device sent input between your last touch and the command. `devlayer clear`, then repeat Step 2 without touching anything else.
- **A key in VIAL does nothing:** make sure you edited the layer the device is bound to (`devlayer list`), and that the key is not Transparent on that layer if you expect it to differ from layer 0.
- **The wheel scrolls in the wrong direction or too slowly:** wheel direction follows the key (wheel up → Volume Up). The distance for one tap is `MOUSE_COUNTS_PER_WHEEL_NOTCH` in `src/config.h` (it only matters when *cursor movement* is mapped to a key or the wheel).
- **The binding disappeared after re-pairing:** bindings are stored by Bluetooth address. A device that uses a different random address after being re-paired counts as a new device, so bind it again.
