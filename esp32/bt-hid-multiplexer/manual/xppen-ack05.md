# XP-Pen ACK05 Mini Keydial: a remappable 13-key keypad

The XP-Pen ACK05 (it calls itself **Shortcut Remote** over Bluetooth) has 10 keys, a dial and a button in the middle of the dial. Out of the box its keys send fixed shortcuts such as `Ctrl+Z`, `Ctrl+Shift+Z` or a bare `Ctrl`. XP-Pen's Windows app gives them other meanings, but only inside that app: the pad itself keeps sending the same shortcuts, and the dial button sends nothing a computer can see.

Connected to the multiplexer, the ACK05 instead works as a **numeric keypad**, where every key, both dial directions and the dial button are keys of their own. You can use it as a keypad as it is, or remap each key to anything in [VIAL](https://vial.rocks/), for the ACK05 only.

---

## What the keys send

The 10 keys are numbered from top left to bottom right.

| Control | Sends |
| --- | --- |
| Dial, turned left (one press per click) | Keypad `-` |
| Dial, turned right (one press per click) | Keypad `+` |
| Button in the middle of the dial | Keypad `Enter` |
| Keys 1–9 | Keypad `1`–`9` |
| Key 10 | Keypad `0` |

Nothing else is needed: once the ACK05 is paired, it sends these.

**Num Lock:** like on a full-size keyboard, the computer reads Keypad `1`–`9` and `0` as End, Down, Page Down, ... when Num Lock is off. Turn Num Lock on, or remap the keys (below), if that gets in the way.

## Pairing

Pair it like any other device (see [How to Pair a Device](../README.md#how-to-pair-a-device)): press the multiplexer's button to start pairing mode, then put the ACK05 into Bluetooth pairing mode (see its manual). The serial console confirms the keypad mode with:

```
[BLE Host] Slot 0 'Shortcut Remote': chord mode (each shortcut is a key of its own)
```

## Remapping the keys

In VIAL, the ACK05's keys are the **numeric keypad** on the right side of the key map: Keypad `-`, `+`, `Enter`, `1`–`9` and `0`. Remap them like any other key.

To change them **for the ACK05 only**, without changing the numeric keypads of your other keyboards, give the ACK05 a layer of its own, as described in [Per-device mapping](per-device-mapping.md):

1. In VIAL's **Layout** tab, set the `Shortcut Remote` dropdown to a free layer, e.g. **Layer 3**.
2. In the **Keymap** tab, select layer 3 and set the keypad keys you want to change. For example:
   - Keypad `-` / `+` (the dial) → **Volume Down** / **Volume Up**
   - Keypad `1` (key 1) → `Ctrl+Z`, Keypad `2` (key 2) → `Ctrl+Shift+Z`
   - Keypad `Enter` (the dial button) → `MO(4)`, so that holding it switches the keys to another set (layer 4) of your choice, like the "layers" of XP-Pen's Windows app
3. Leave the other keys **Transparent**: they keep sending their keypad keys.

The dial button can be held: it is down for as long as you hold it, so it works for `MO(n)` and other hold keys as well as for taps.

## Limitations

- **Several keys at once:** holding keys together works, but a few combinations cannot be told apart, because the pad merges the shortcuts of the keys held into one:
  - Pressing key 5 (Ctrl) while a key with Ctrl is held (keys 1, 2, 7, 8, 10, the dial), or key 4 (Shift) while key 10 is held, changes nothing the pad sends, so it is not seen.
  - Keys 4 and 8 held together look the same as key 10, and keys 5 and 1 the same as key 1. The multiplexer goes by which key was already held: holding 4 and then pressing 8 gives Keypad `4` + `8`, while pressing 10 alone gives Keypad `0`. Pressing such a pair at exactly the same time gives the single key (Keypad `0`, Keypad `1`).
- **Right after the ACK05 connects** (the first second), the dial button is ignored.
- **If the dial button is held for more than 30 seconds**, it is released by itself.
- The keypad mode is chosen by the Bluetooth name `Shortcut Remote`. A different device with exactly that name is treated the same way, but anything it sends that is not one of the ACK05's shortcuts passes through unchanged.

---

## Technical details

### The HID descriptor

The ACK05's BLE HID report descriptor (286 bytes, read with the console's `desc` command) is a generic XP-Pen/UGEE one with four input reports:

| Report ID | Collection | Payload |
| --- | --- | --- |
| 9 | Mouse: 3 buttons, **absolute** 16-bit X/Y, pressure | 7 bytes |
| 1 | Mouse: 5 buttons, **relative** 16-bit X/Y, wheel, AC Pan | 7 bytes |
| 6 | Keyboard: 8 modifier bits + 6 keycodes, **no reserved byte** | 7 bytes |
| 7 | Digitizer pen: tip/barrel/eraser bits, absolute X/Y, pressure, tilt | 9 bytes |

Only report 6 was seen in use. It differs from the boot keyboard layout (ID 1, `[mods, reserved, k1..k6]`) in both its ID and its layout, so `main/hid_descriptor.cpp` reads the keyboard report's ID and field offsets from the descriptor, and picks the relative mouse (ID 1) over the absolute one (ID 9).

### What the pad sends

Captured from the device with the console's `reports on`:

| Control | Report 6 (`mods key ...`) | Shortcut |
| --- | --- | --- |
| Dial left | `01 56` | Ctrl + Keypad `-` |
| Dial right | `01 57` | Ctrl + Keypad `+` |
| Key 1 | `01 12` | Ctrl + O |
| Key 2 | `01 11` | Ctrl + N |
| Key 3 | `00 3E` | F5 |
| Key 4 | `02 00` | Left Shift |
| Key 5 | `01 00` | Left Ctrl |
| Key 6 | `04 00` | Left Alt |
| Key 7 | `01 16` | Ctrl + S |
| Key 8 | `01 1D` | Ctrl + Z |
| Key 9 | `00 2C` | Space |
| Key 10 | `03 1D` | Ctrl + Shift + Z |
| Dial button | `00 00` on press, `00 00` on release | (nothing) |

- Every press is **one report with the whole shortcut**: the modifiers never arrive ahead of the key. The release is an all-zero report.
- **Keys held together are merged into one report:** their modifier bits are ORed and their keys listed side by side, and releasing one key sends the report of the keys still held. Captured:

  | Action | Reports |
  | --- | --- |
  | Hold 8, tap 9 | `01 1D` → `01 1D 2C` → `01 1D` → `00` |
  | Hold 9, tap 8 | `00 2C` → `01 1D 2C` → `00 2C` → `00` |
  | Hold 4, tap 8 | `02 00` → `03 1D` → `02 00` → `00` |
  | Hold 8, tap 4 | `01 1D` → `03 1D` → `01 1D` → `00` |
  | Hold 1, tap 2 | `01 12` → `01 12 11` → `01 12` → `00` |
  | Hold 5, tap 1 | `01 00` → `01 12` → `01 00` → `00` |
  | 8 and 9 at once | `01 1D 2C` → `00 2C` → `00` |
- A dial click is a press and a release about 1–2 ms apart.
- The dial button sends an all-zero report when pressed and another when released (both verified with 3–4 s holds). Since no key changes state, the computer sees nothing when the pad is connected to it directly.
- The pad does not change what it sends when the dial button is pressed: the "layers" of XP-Pen's Windows app are done by the app.

### Why plain remapping is not enough

The multiplexer's virtual matrix has one position per keycode and one per modifier bit. On the ACK05, the Left Ctrl position is shared by 8 of the 12 shortcuts, and the Z position by keys 8 and 10. Remapping a position therefore changes several physical keys, and the dial button has no position at all.

### Chord mode

`main/chord_mode.cpp` holds a per-device table of shortcuts (`ChordProfile`), chosen by the device name when the keyboard report is found in the descriptor (`resolve_input_reports()` in `main/ble_hid_host.cpp`). For such a device, each keyboard report goes through `chord_translate()` before it reaches the multiplexer:

- **The report is split back into the shortcuts of the keys held** (`decode()`): it looks for the set of table entries whose modifiers, ORed, are exactly the report's modifiers and whose keys are exactly the report's keys. The report becomes those entries' keypad keys with no modifiers. If no set adds up to the report, it passes through unchanged. Because a press is always a single complete report, no timing or waiting is involved.
- **When several sets add up to the report** (e.g. `03 1D` is key 10, or keys 4 + 8), the one that differs in the fewest keys from the set held before wins, since keys go down and up one at a time. Among those, a set where every entry adds something to the report wins over one where an entry could be left out. So `03 1D` is key 10 from nothing held, but 4 + 8 when 4 or 8 was held. The held set is kept in `ChordState::held_entries`, one bit per table entry (at most 16 entries, `CHORD_MAX_ENTRIES`). The search tries every subset of the entries that fit in the report, which is a handful for this pad.
- **The dial button:** an all-zero report after a non-empty one is a release. An all-zero report while nothing is held is the dial button, and toggles Keypad `Enter`. Releasing a key while the button is held therefore leaves the button down.
- **Connection grace:** in the first second after `HID_SERVICE_CONNECTED` (`CHORD_CONNECT_GRACE_MS`), an all-zero report while nothing is held is ignored. It may be the release of the key that woke the pad up, whose press was sent before the connection.
- **Lost release:** if the button stays down for 30 s (`CHORD_IDLE_BUTTON_TIMEOUT_MS`), `BleHidHost::expireChordButtons()`, called from the 1 s heartbeat timer in `main/app_task.cpp`, releases it. A lost release would otherwise also swap the button's press and release from then on.
- On disconnect, the slot's state, chord state included, is cleared along with the multiplexer's keys for the device.

The translated keys enter the virtual matrix at the keypad positions (`0x56`–`0x62`), which is why they show up on VIAL's numeric keypad and can be remapped there.

### Adding another device

To support another pad that sends fixed shortcuts, add a `ChordEntry` table and a `ChordProfile` with its Bluetooth name to `PROFILES` in `main/chord_mode.cpp`. Capture what each key sends first: run `reports on` on the console (it turns itself off after 15 s) and press the keys one by one. Then add the captured reports to `test/chord_mode_test.cpp` and run `test/run-host-test.sh`.

### Files

| File | Role |
| --- | --- |
| `main/hid_descriptor.{h,cpp}` | Finds the keyboard report's ID and layout, and the relative mouse report, in the descriptor |
| `main/chord_mode.{h,cpp}` | The ACK05 table, `chord_translate()`, `chord_expire()` |
| `main/ble_hid_host.cpp` | Chooses the profile, runs reports through `chord_translate()`, `expireChordButtons()` |
| `main/app_task.cpp` | Calls `expireChordButtons()` from the heartbeat timer |
| `test/hid_descriptor_test.cpp` | Tests with the ACK05's real descriptor |
| `test/chord_mode_test.cpp` | Tests with the ACK05's captured reports |
