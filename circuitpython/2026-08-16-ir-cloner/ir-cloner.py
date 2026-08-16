#!/usr/bin/env circuit-run
"""
Universal IR Signal Cloner & Blaster for Raspberry Pi Pico.

Records any infrared remote control signal via an IR receiver, identifies the protocol
and optimal carrier frequency, and replays the recorded signal when the trigger button is pressed.

Hardware Connections:
- GP19 (Pin 25): IR Receiver DATA/OUT pin
- GP20 (Pin 26): IR Blaster/Transmitter DATA/IN pin
- GP17 (Pin 22): Push Button (Active LOW with internal pull-up) -> Replay Trigger
- GND  (Pin 23 / 28): Common Ground for receiver, blaster, and button
- 3V3  (Pin 36): VCC Power for receiver and blaster (or 5V VBUS on Pin 40 for higher IR LED power)
"""

import array
import time
import board
import digitalio
import pulseio
import supervisor

# Disable auto-reload on filesystem writes to prevent spurious restarts from OS daemons
supervisor.runtime.autoreload = False

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

# Known Sony SIRC 7-bit command codes
SONY_BUTTON_MAP: dict[int, str] = {
    0x00: "1",
    0x01: "2",
    0x02: "3",
    0x03: "4",
    0x04: "5",
    0x05: "6",
    0x06: "7",
    0x07: "8",
    0x08: "9",
    0x09: "0",
    0x0A: "11 / ENTER",
    0x0B: "12",
    0x10: "Channel Up / CH+",
    0x11: "Channel Down / CH-",
    0x12: "Volume Up / VOL+",
    0x13: "Volume Down / VOL-",
    0x14: "Mute",
    0x15: "Power",
    0x18: "Power ON",
    0x19: "Power OFF",
    0x24: "Up Arrow",
    0x25: "Down Arrow",
    0x33: "Left Arrow",
    0x34: "Right Arrow",
    0x65: "Enter / OK",
    0x26: "Input / TV/Video",
    0x3A: "Menu / Home",
    0x58: "Back / Return",
    0x5B: "Display / Info",
}

# Known Sony SIRC device address codes
SONY_DEVICE_MAP: dict[int, str] = {
    0x01: "TV",
    0x02: "VCR 1",
    0x03: "VCR 2",
    0x04: "VCR 3",
    0x06: "Laserdisc",
    0x0C: "Surround Receiver",
    0x10: "Audio Receiver / Amp",
    0x11: "CD Player",
    0x12: "MiniDisc",
    0x13: "DVD Player",
}


class DebouncedButton:
    """Debounces a digital push button connected with an internal pull-up resistor."""

    def __init__(self, pin: board.Pin, debounce_delay_s: float = 0.02) -> None:
        self.io: digitalio.DigitalInOut = digitalio.DigitalInOut(pin)
        self.io.direction = digitalio.Direction.INPUT
        self.io.pull = digitalio.Pull.UP
        self.debounce_delay_s: float = debounce_delay_s

        self.is_pressed: bool = not self.io.value
        self._last_raw_value: bool = self.io.value
        self._last_change_time: float = time.monotonic()

    def update(self) -> bool:
        """
        Polls button pin state with debouncing.
        Returns True if the debounced button state changed, False otherwise.
        """
        raw_val: bool = self.io.value
        now: float = time.monotonic()

        if raw_val != self._last_raw_value:
            self._last_raw_value = raw_val
            self._last_change_time = now

        if (now - self._last_change_time) >= self.debounce_delay_s:
            debounced_pressed: bool = not raw_val
            if debounced_pressed != self.is_pressed:
                self.is_pressed = debounced_pressed
                return True

        return False


def decode_nec(pulses: list[int]) -> tuple[int, int, int, bool] | None:
    """
    Attempts to decode a list of pulse durations (in microseconds) as an NEC protocol packet.
    Returns (hex_code_32bit, address, command, is_repeat) if successfully decoded, or None.
    """
    if len(pulses) < 2:
        return None

    # Scan for NEC leader pulse (~9000us mark)
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

    data_bits: int = 0
    for i in range(32):
        space_idx: int = leader_idx + 2 + (i * 2) + 1
        if space_idx >= len(pulses):
            if i == 31:
                cmd_byte: int = (data_bits >> 16) & 0xFF
                expected_bit: int = ((~cmd_byte) >> 7) & 0x01
                data_bits |= expected_bit << 31
                break
            return None

        space_len: int = pulses[space_idx]
        if 250 < space_len < 900:
            # Logical 0
            pass
        elif 900 <= space_len < 2400:
            # Logical 1
            data_bits |= 1 << i
        else:
            return None

    byte0: int = data_bits & 0xFF
    byte1: int = (data_bits >> 8) & 0xFF
    byte2: int = (data_bits >> 16) & 0xFF
    byte3: int = (data_bits >> 24) & 0xFF

    full_hex_code: int = (byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3

    if (byte2 ^ byte3) == 0xFF:
        address: int = byte0 if (byte0 ^ byte1) == 0xFF else ((byte1 << 8) | byte0)
        command: int = byte2
        return (full_hex_code, address, command, False)

    return (full_hex_code, byte0, byte2, False)


def decode_sony_sirc(pulses: list[int]) -> tuple[int, int, int, int] | None:
    """
    Attempts to decode a list of pulse durations (in microseconds) as a Sony SIRC protocol packet.
    Returns (raw_code, device_address, command, bit_length) if successfully decoded, or None.
    """
    if len(pulses) < 3:
        return None

    # Scan for Sony leader pulse (~2400us mark)
    leader_idx: int = -1
    for idx in range(len(pulses) - 1):
        if 1800 < pulses[idx] < 3000:
            leader_idx = idx
            break

    if leader_idx == -1:
        return None

    leader_space: int = pulses[leader_idx + 1]
    if not (300 < leader_space < 1000):
        return None

    data_bits: int = 0
    bit_count: int = 0

    for bit_idx in range(20):
        pulse_idx: int = leader_idx + 2 + (bit_idx * 2)
        if pulse_idx >= len(pulses):
            break

        pulse_len: int = pulses[pulse_idx]
        if 300 <= pulse_len < 900:
            # Logical 0 (~600us pulse)
            bit_count += 1
        elif 900 <= pulse_len < 1600:
            # Logical 1 (~1200us pulse)
            data_bits |= 1 << bit_idx
            bit_count += 1
        else:
            break

    if bit_count not in (12, 15, 20):
        return None

    command: int = data_bits & 0x7F
    if bit_count == 12:
        device_addr: int = (data_bits >> 7) & 0x1F
    elif bit_count == 15:
        device_addr = (data_bits >> 7) & 0xFF
    else:
        device_addr = (data_bits >> 7) & 0x1FFF

    return (data_bits, device_addr, command, bit_count)


def blast_ir_signal(pin: board.Pin, pulses: list[int], frequency: int = 38000, repeat_count: int = 1) -> None:
    """
    Transmits an array of raw microsecond pulse durations over the specified GPIO pin
    using hardware carrier modulation.
    """
    pulse_arr: array.array = array.array("H", pulses)
    for i in range(repeat_count):
        emitter: pulseio.PulseOut = pulseio.PulseOut(pin, frequency=frequency, duty_cycle=2**15)
        try:
            emitter.send(pulse_arr)
        finally:
            emitter.deinit()

        if i < repeat_count - 1:
            time.sleep(0.045)  # Standard 45ms inter-frame gap


def main() -> None:
    """Runs the interactive IR cloner and blaster loop."""
    print("==================================================")
    print("Universal IR Cloner & Blaster Ready")
    print("Listening on GP19 (Pin 25)...")
    print("Blaster Pin   : GP20 (Pin 26)")
    print("Trigger Button: GP17 (Pin 22)")
    print("==================================================")
    print("\nStep 1: Point your remote at the receiver and press any key to record.")

    # Initialize PulseIn on GP19 for signal recording
    receiver: pulseio.PulseIn = pulseio.PulseIn(board.GP19, maxlen=300, idle_state=True)

    # Initialize Trigger Button on GP17
    button: DebouncedButton = DebouncedButton(board.GP17)

    # Optional onboard LED indicator
    led: digitalio.DigitalInOut | None = None
    try:
        led = digitalio.DigitalInOut(board.LED)
        led.direction = digitalio.Direction.OUTPUT
        led.value = False
    except Exception:
        pass

    # Storage for the most recently cloned signal
    cloned_pulses: list[int] | None = None
    cloned_carrier_freq: int = 38000
    cloned_protocol_name: str = "None"
    cloned_repeat_count: int = 1

    while True:
        # Check if new IR pulses have arrived at receiver
        if len(receiver) > 0:
            # Idle detection to ensure full packet has finished arriving
            last_count: int = len(receiver)
            while True:
                time.sleep(0.015)
                current_count: int = len(receiver)
                if current_count == last_count:
                    break
                last_count = current_count

            receiver.pause()
            pulse_count: int = len(receiver)
            raw_pulses: list[int] = [receiver[i] for i in range(pulse_count)]
            receiver.clear()
            receiver.resume()

            if pulse_count > 4:  # Ignore short noise glitches
                cloned_pulses = raw_pulses

                # Check if signal matches known protocols to optimize carrier frequency and repeat behavior
                nec_result = decode_nec(raw_pulses)
                sony_result = decode_sony_sirc(raw_pulses)

                print("\n==================================================")
                print(f"[RECORDED] Captured new signal ({pulse_count} transitions)")

                if sony_result is not None:
                    raw_code, device_addr, cmd, bit_len = sony_result
                    device_name: str = SONY_DEVICE_MAP.get(device_addr, "Unknown Device")
                    button_name: str = SONY_BUTTON_MAP.get(cmd, "Unknown Key")
                    cloned_carrier_freq = 40000
                    cloned_protocol_name = f"Sony SIRC ({bit_len}-bit)"
                    cloned_repeat_count = 3  # Sony remotes send 3 bursts per keypress
                    print(f"  Protocol    : {cloned_protocol_name}")
                    print(f"  Hex Code    : 0x{raw_code:04X}")
                    print(f"  Device      : 0x{device_addr:02X} -> [{device_name}]")
                    print(f"  Command     : 0x{cmd:02X} ({cmd}) -> [{button_name}]")
                    print(f"  Carrier Freq: 40 kHz (Transmitting 3x bursts)")
                elif nec_result is not None:
                    hex_code, addr, cmd, is_repeat = nec_result
                    button_name = NEC_BUTTON_MAP.get(cmd, "Unknown Key")
                    cloned_carrier_freq = 38000
                    cloned_protocol_name = "NEC"
                    cloned_repeat_count = 1
                    print(f"  Protocol    : {cloned_protocol_name}")
                    print(f"  Hex Code    : 0x{hex_code:08X}")
                    print(f"  Address     : 0x{addr:02X}")
                    print(f"  Command     : 0x{cmd:02X} -> [{button_name}]")
                    print(f"  Carrier Freq: 38 kHz")
                else:
                    cloned_carrier_freq = 38000
                    cloned_protocol_name = "Raw / Custom"
                    cloned_repeat_count = 1
                    print(f"  Protocol    : {cloned_protocol_name}")
                    print(f"  Carrier Freq: 38 kHz (Default)")

                print("--> READY: Press the button on GP17 to BLAST this signal!")
                print("==================================================")

                # Flash LED twice to confirm capture
                if led is not None:
                    led.value = True
                    time.sleep(0.08)
                    led.value = False
                    time.sleep(0.08)
                    led.value = True
                    time.sleep(0.08)
                    led.value = False

        # Check if the trigger button on GP17 was pressed
        if button.update() and button.is_pressed:
            if cloned_pulses is None:
                print("\n[WARNING] No signal recorded yet! Point a remote at the receiver first.")
            else:
                print(f"\n[BLASTING] Transmitting '{cloned_protocol_name}' on GP20 @ {cloned_carrier_freq // 1000} kHz...")
                if led is not None:
                    led.value = True

                blast_ir_signal(
                    board.GP20,
                    cloned_pulses,
                    frequency=cloned_carrier_freq,
                    repeat_count=cloned_repeat_count,
                )

                if led is not None:
                    led.value = False
                print("[DONE] Signal transmitted successfully.")

        time.sleep(0.005)


if __name__ == "__main__":
    main()
