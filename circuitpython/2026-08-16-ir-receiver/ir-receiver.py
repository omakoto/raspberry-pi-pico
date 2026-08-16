#!/usr/bin/env circuit-run
"""
IR Receiver & Pulse Logger for Raspberry Pi Pico.

Captures raw IR pulses and decodes remote control signals (such as standard NEC protocol).

Wiring Guide:
1. Bare 3-pin IR Receiver (e.g. VS1838B / TSOP38238):
   (With the dome/lens facing you and pins pointing down, from LEFT to RIGHT):
   - Pin 1 (OUT / DATA) -> Pico GP19 (Physical Pin 25)
   - Pin 2 (GND)        -> Pico GND (Physical Pin 23 or 28)
   - Pin 3 (VCC)        -> Pico 3V3 OUT (Physical Pin 36)

2. PCB Module (with 3-pin header):
   - 'S' / 'OUT' / 'DAT' -> Pico GP19 (Physical Pin 25)
   - '+' / 'VCC'         -> Pico 3V3 OUT (Physical Pin 36)
   - '-' / 'GND'         -> Pico GND (Physical Pin 23 or 28)
"""

import time
import board
import digitalio
import pulseio

# When True, all captured raw pulse timings are printed; when False, output is truncated to the first 32 pulses
VERBOSE: bool = True

# Known NEC remote button codes commonly found on 21-key mini IR remotes
NEC_BUTTON_MAP: dict[int, str] = {
    0x45: "CH- / POWER",
    0x46: "CH / MODE",
    0x47: "CH+ / MUTE",
    0x44: "PREV / <<",
    0x40: "NEXT / >>",
    0x43: "PLAY/PAUSE / >||",
    0x07: "VOL-",
    0x15: "VOL+",
    0x09: "EQ",
    0x16: "0",
    0x19: "100+",
    0x0D: "200+",
    0x0C: "1",
    0x18: "2",
    0x5E: "3",
    0x08: "4",
    0x1C: "5",
    0x5A: "6",
    0x42: "7",
    0x52: "8",
    0x4A: "9",
}


def decode_nec(pulses: list[int]) -> tuple[int, int, int, bool] | None:
    """
    Attempts to decode a list of pulse durations (in microseconds) as an NEC protocol packet.
    Returns (hex_code_32bit, address, command, is_repeat) if successfully decoded, or None.
    """
    if len(pulses) < 2:
        return None

    # Scan for NEC leader pulse (~9000us) to tolerate any initial noise glitches
    leader_idx: int = -1
    for idx in range(len(pulses) - 1):
        if 7000 < pulses[idx] < 12000:
            leader_idx = idx
            break

    if leader_idx == -1:
        return None

    leader_pulse: int = pulses[leader_idx]
    leader_space: int = pulses[leader_idx + 1]

    # NEC Repeat Code: ~9000us pulse followed by ~2250us space
    if 1600 < leader_space < 3200:
        return (0, 0, 0, True)

    # Standard NEC Data Frame: ~9000us pulse followed by ~4500us space
    if not (3500 < leader_space < 5500):
        return None

    # Decode 32 data bits (each bit has a mark pulse followed by a space)
    data_bits: int = 0
    for i in range(32):
        space_idx: int = leader_idx + 2 + (i * 2) + 1
        if space_idx >= len(pulses):
            # If the line returned to idle before the 32nd space was captured, deduce bit 31 from complement
            if i == 31:
                cmd_byte: int = (data_bits >> 16) & 0xFF
                expected_bit: int = ((~cmd_byte) >> 7) & 0x01
                data_bits |= expected_bit << 31
                break
            return None

        space_len: int = pulses[space_idx]
        if 250 < space_len < 900:
            # Logical 0 (~560us space)
            pass
        elif 900 <= space_len < 2400:
            # Logical 1 (~1690us space)
            data_bits |= 1 << i
        else:
            # Out of timing tolerance
            return None

    byte0: int = data_bits & 0xFF
    byte1: int = (data_bits >> 8) & 0xFF
    byte2: int = (data_bits >> 16) & 0xFF
    byte3: int = (data_bits >> 24) & 0xFF

    # 32-bit hex code (Byte0.Byte1.Byte2.Byte3)
    full_hex_code: int = (byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3

    # Verify standard NEC complement check: byte2 == ~byte3
    if (byte2 ^ byte3) == 0xFF:
        address: int = byte0 if (byte0 ^ byte1) == 0xFF else ((byte1 << 8) | byte0)
        command: int = byte2
        return (full_hex_code, address, command, False)

    # Extended NEC (address without complement, or slight bit errors)
    return (full_hex_code, byte0, byte2, False)


def main() -> None:
    """Initializes the IR receiver on GP19 and continuously logs pulse timings."""
    print("==================================================")
    print("Raspberry Pi Pico IR Receiver & Pulse Logger")
    print("Listening on GP19 (Pin 25)...")
    print("Point your IR remote at the receiver and press any key.")
    print("==================================================")

    # Initialize PulseIn on GP19.
    # IR receivers pull the line HIGH when idle, and pulse LOW when carrier is detected.
    pulses: pulseio.PulseIn = pulseio.PulseIn(board.GP19, maxlen=200, idle_state=True)

    # Optional onboard LED indicator
    led: digitalio.DigitalInOut | None = None
    try:
        led = digitalio.DigitalInOut(board.LED)
        led.direction = digitalio.Direction.OUTPUT
        led.value = False
    except Exception:
        pass

    last_received_time: float = 0.0

    while True:
        # Wait until pulses start arriving
        if len(pulses) == 0:
            time.sleep(0.005)
            continue

        # Wait until the burst finishes transmitting (idle detection)
        last_count: int = len(pulses)
        while True:
            time.sleep(0.015)
            current_count: int = len(pulses)
            if current_count == last_count:
                break
            last_count = current_count

        pulses.pause()
        pulse_count: int = len(pulses)
        raw_pulses: list[int] = [pulses[i] for i in range(pulse_count)]
        pulses.clear()
        pulses.resume()

        # Flash onboard LED briefly
        if led is not None:
            led.value = True

        now: float = time.monotonic()
        time_since_last: float = now - last_received_time
        last_received_time = now

        print(f"\n--- IR Signal Captured ({pulse_count} transitions, +{time_since_last:.2f}s) ---")

        # Format and display raw pulse timings in microseconds
        print(f"  Raw Pulse Durations (us, count={len(raw_pulses)}):")
        # Print pulse values in chunks of 8 for clear terminal formatting
        chunk_size: int = 8
        limit: int = len(raw_pulses) if VERBOSE else min(len(raw_pulses), 32)
        for i in range(0, limit, chunk_size):
            chunk: list[int] = raw_pulses[i:i + chunk_size]
            formatted_chunk: str = ", ".join(f"{p:5d}" for p in chunk)
            print(f"    [{i:2d}..{i+len(chunk)-1:2d}]: {formatted_chunk}")
        if not VERBOSE and len(raw_pulses) > limit:
            print(f"    ... (+ {len(raw_pulses) - limit} more pulses)")

        # Attempt NEC protocol decoding and print decoded hex code after raw pulses
        nec_result: tuple[int, int, int, bool] | None = decode_nec(raw_pulses)
        print("  Decoded Signal:")
        if nec_result is not None:
            hex_code, addr, cmd, is_repeat = nec_result
            if is_repeat:
                print("    Protocol : NEC (REPEAT)")
            else:
                button_name: str = NEC_BUTTON_MAP.get(cmd, "Unknown Key")
                print("    Protocol : NEC")
                print(f"    Hex Code : 0x{hex_code:08X}")
                print(f"    Address  : 0x{addr:02X}")
                print(f"    Command  : 0x{cmd:02X} -> [{button_name}]")
        else:
            print("    Protocol : Raw / Non-standard")

        if led is not None:
            led.value = False


if __name__ == "__main__":
    main()
