# Per-device mapping: remap one device without affecting the others

By default the keymap you edit in [VIAL](https://vial.rocks/) is shared by every keyboard and mouse connected to the multiplexer. If you remap the mouse wheel to volume, *every* mouse's wheel becomes volume.

**Per-device mapping** fixes that. You *bind* one Bluetooth device to one keymap layer. That device is then looked up on its own layer first, so whatever you put there applies to that device only. Everything you leave **Transparent** on that layer behaves as before.

You do all of it in VIAL: bind the device in the **Layout** tab, then edit its layer in the **Keymap** tab.

Example used throughout this manual: a Logitech device with a vertical wheel, a horizontal wheel and four side buttons, where the vertical wheel should control the volume, while the wheels of your other mice keep scrolling.

---

## What you need

- The multiplexer flashed and running (see the main [README](../README.md)).
- The device paired with the multiplexer (see [How to Pair a Device](../README.md#how-to-pair-a-device)). It does not need to be switched on to be bound.
- VIAL access from Chrome or another Chromium-based browser (on Linux, run `~/cbin/setup/config-hidraw-permission` once, see [Keymapping with VIAL](../README.md#keymapping-with-vial)).

---

## Step 1. Pick a layer for the device

The keymap has 8 layers (0–7). Layer 0 is the base layer shared by all devices. Choose one of layers **1 to 7** for the device. Each layer can be used by several devices; devices bound to the same layer also share their layer keys (see "A layer key" below). Pick a layer you are not using for `MO(n)` and other layer keys: while such a key selects that layer, the devices it applies to see the bound device's mappings too.

This manual uses **layer 3**.

## Step 2. Bind the device to the layer

1. Open (or reload) [vial.rocks](https://vial.rocks/), click **Start** and select the multiplexer. VIAL reads the list of paired devices when it connects, so reload it after pairing a new device.
2. Open the **Layout** tab. It has one dropdown per paired device, labelled with the device's name. Two devices with the same name get the end of their Bluetooth address added, e.g. `MX Dialpad (7B:44)`.
3. Set the device's dropdown to **Layer 3**.

The binding applies at once and is saved in flash, by the device's Bluetooth address, so it survives reconnects and reboots. The OLED confirms it (`<name>: layer 3`). Nothing changes yet, because layer 3 is still all Transparent.

The list also contains paired devices that are switched off, so a device can be bound before it connects.

A binding is kept when its device stops being paired; see [Devices that are no longer paired](#devices-that-are-no-longer-paired).

**Layout files:** loading a saved layout file in VIAL restores these dropdowns too, which rebinds devices. It matches them by their position in the list, so a file saved with different pairings can bind the wrong devices. Check the Layout tab after loading one.

## Step 3. Edit the layer in VIAL

1. Open the **Keymap** tab and select **layer 3** with VIAL's layer selector.
2. Find the **bottom row** of the key map, the mouse row (below the F13–F24 row). From left to right it contains:
   - **8 mouse buttons**, in order: button 1 (left), 2 (right), 3 (middle), 4, 5, 6, 7, 8.
   - A gap, then **8 movement keys**: cursor up, cursor down, cursor left, cursor right, **wheel up, wheel down, wheel left, wheel right**.
3. Click the key you want to change, then pick a keycode from VIAL's keycode list (the volume keys are in its media section). For the example:
   - the **wheel up** key → **Volume Up** (`KC_VOLU`)
   - the **wheel down** key → **Volume Down** (`KC_VOLD`)

   Changes apply immediately and are saved to flash half a second after your last edit.
4. Leave everything else **Transparent** (`KC_TRNS`, which is what every key on layers 1–7 starts as): those inputs behave as on layer 0.

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
- **A keyboard key**, a modifier or a modified key such as `LSFT(KC_A)`. A key on a *wheel or movement* position is **tapped once per wheel notch** (for movement, once per 24 counts of motion, see `MOUSE_COUNTS_PER_WHEEL_NOTCH` in `main/config.h`).
- **Media and browser keys:** volume (`KC_MUTE`, `KC_VOLU`, `KC_VOLD`), play/pause, next/previous track, browser back/forward and so on, and Power/Sleep/Wake.
- **Mouse keys** on a key or button: cursor (`KC_MS_U` and so on) or wheel (`KC_WH_U` and so on) moves or scrolls while held, speeding up like QMK's mouse keys; hold a key mapped to `KC_ACL0`/`KC_ACL1`/`KC_ACL2` for a fixed slow/medium/fast speed. See the [README](../README.md).
- **A layer key** (`MO(1)`, `TG(1)`, `TO(1)` and so on) on one of the device's buttons. While the layer is selected, it is looked up first, then the device's layer, then layer 0. This makes "hold a button and the wheel does something else" possible for just this device. A layer key on a bound device only applies to the devices bound to the same layer. It does not change the layer of other devices, and layer keys on unbound devices do not change the layer of bound devices.
- **A macro** (`M0`–`M63`, edited in VIAL's **Macros** tab) on a key or button. Macros on wheel or movement positions do nothing.
- `KC_NO` to disable an input.

Not available: tap dance, mod-tap.

## Changing or removing a binding

All of these are in VIAL's **Layout** tab:

| To do this | Do this |
| --- | --- |
| Move a device to another layer | pick the other layer in its dropdown |
| Remove a binding | pick **No binding** |
| Remove the binding of a device that is no longer paired | clear its **Unpaired** checkbox |
| See which device is bound to which layer | look at the dropdowns and checkboxes |

Every paired device (up to 8) can be bound.

## Devices that are no longer paired

A binding is not removed when its device stops being paired. That happens when you remove the device's pairing, when holding the button for 8 s removes all pairings, or when pairing a ninth device drops the pairing of the device used least recently (the multiplexer keeps 8 pairings).

- The binding then shows up at the end of the **Layout** tab as a checkbox, `Unpaired: <name> (layer N)`, checked.
- **To remove it,** clear the checkbox. Checking it again before you reload VIAL puts it back.
- **If the device is paired again** with the same Bluetooth address, it gets its layer back by itself and returns to the dropdowns.
- Up to 8 of these are kept. When a ninth device stops being paired, the binding of the device that was unpaired longest ago is removed.

## Troubleshooting

- **The device is not in the Layout tab:** VIAL reads the list when it connects. Reload vial.rocks after pairing the device. Only paired devices are listed.
- **Two dropdowns have the same name:** they get the end of their Bluetooth address added. The OLED shows the name of the device used last, and the serial console's `devices` command shows the addresses (see the [README](../README.md#serial-console)).
- **A key in VIAL does nothing:** make sure you edited the layer the device is bound to (see its dropdown in the Layout tab), and that the key is not Transparent on that layer if you expect it to differ from layer 0.
- **The wheel scrolls in the wrong direction or too slowly:** wheel direction follows the key (wheel up → Volume Up). The distance for one tap is `MOUSE_COUNTS_PER_WHEEL_NOTCH` in `main/config.h` (it only matters when *cursor movement* is mapped to a key or the wheel).
- **The binding disappeared after re-pairing:** bindings are stored by Bluetooth address. A device that uses a different random address after being re-paired counts as a new device, so bind it again. Its old binding stays as an `Unpaired` checkbox until you clear it.
