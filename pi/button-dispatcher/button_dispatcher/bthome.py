"""BTHome v2 decoding for BLE buttons and remotes.

A BTHome advertisement carries its payload as service data under the 16-bit UUID 0xFCD2. The
first byte describes the packet (version, encryption, trigger-based); the rest is a sequence of
objects, each an id byte followed by a fixed-length little-endian value.

Only what button-like devices send is interpreted (packet id, battery, button events, the wheel
"dimmer" object and the channel). Other objects are skipped by length so that devices which mix
them in still decode; an object id this module doesn't know ends decoding, because object lengths
depend on the id and the rest of the payload can't be split safely.

Measured behavior of the Shelly BLU remotes this was built for is in
~/history/reports/electronics/shelly-remote-switches.md.
"""

from __future__ import annotations

import dataclasses
from typing import Optional

SERVICE_UUID_16 = 0xFCD2
# The form bleak uses for a 16-bit service UUID in advertisement data.
SERVICE_UUID = "0000fcd2-0000-1000-8000-00805f9b34fb"

OBJ_PACKET_ID = 0x00
OBJ_BATTERY = 0x01
OBJ_BUTTON = 0x3A
OBJ_DIMMER = 0x3C
OBJ_CHANNEL = 0x60

# Value length in bytes for each object id. Only the ids above are interpreted; the rest are
# listed so their length is known and decoding can continue past them.
OBJECT_LENGTHS: dict[int, int] = {
    0x00: 1,   # packet id
    0x01: 1,   # battery %
    0x02: 2,   # temperature
    0x03: 2,   # humidity
    0x04: 3,   # pressure
    0x05: 3,   # illuminance
    0x08: 2,   # dew point
    0x0C: 2,   # voltage
    0x10: 1,   # power
    0x11: 1,   # opening
    0x15: 1,   # battery low
    0x21: 1,   # motion
    0x2D: 1,   # window
    0x2E: 1,   # humidity (8 bit)
    0x2F: 1,   # moisture (8 bit)
    0x3A: 1,   # button event
    0x3C: 2,   # dimmer: event, steps
    0x3F: 2,   # rotation
    0x40: 2,   # distance
    0x45: 2,   # temperature (0.1)
    0x60: 1,   # channel
}

BUTTON_EVENT_NAMES: dict[int, str] = {
    0x01: "press",
    0x02: "double_press",
    0x03: "triple_press",
    0x04: "long_press",
    0x05: "long_double_press",
    0x06: "long_triple_press",
    0x80: "hold_press",
}

# The dimmer object's event byte: BTHome calls these rotate left / rotate right.
WHEEL_DIRECTION_NAMES: dict[int, str] = {0x01: "left", 0x02: "right"}

WHEEL_EVENT = "turn"


@dataclasses.dataclass
class Packet:
    """A decoded BTHome service-data payload."""
    version: int
    encrypted: bool
    trigger_based: bool
    packet_id: Optional[int] = None
    battery: Optional[int] = None
    channel: Optional[int] = None
    # Event code of each button slot, in slot order (slot 1 first); 0 means no event.
    buttons: list[int] = dataclasses.field(default_factory=list)
    wheel_event: Optional[int] = None
    wheel_steps: int = 0
    # False when decoding stopped early at an unknown or truncated object.
    complete: bool = True
    raw: bytes = b""


@dataclasses.dataclass(frozen=True)
class InputEvent:
    """One user action in a packet: a button event, or a wheel turn."""
    # "button:N" (N counts from 1) or "wheel:left" / "wheel:right".
    input: str
    # A key of BUTTON_EVENT_NAMES' values, or WHEEL_EVENT for the wheel.
    event: str
    # For the wheel, the step count the device reported (0 in the "start" packet of a turn).
    steps: int = 0


def decode(data: bytes) -> Packet:
    """Decodes a BTHome v2 payload; raises ValueError if it is empty."""
    if not data:
        raise ValueError("empty BTHome payload")
    info = data[0]
    packet = Packet(version=info >> 5, encrypted=bool(info & 1), trigger_based=bool(info & 4),
                    raw=bytes(data))
    if packet.encrypted:
        # The payload is sealed with a per-device key, so none of it can be read without it.
        return packet

    i = 1
    while i < len(data):
        oid = data[i]
        length = OBJECT_LENGTHS.get(oid)
        if length is None or i + 1 + length > len(data):
            packet.complete = False
            break
        value = data[i + 1:i + 1 + length]
        if oid == OBJ_PACKET_ID:
            packet.packet_id = value[0]
        elif oid == OBJ_BATTERY:
            packet.battery = value[0]
        elif oid == OBJ_CHANNEL:
            packet.channel = value[0]
        elif oid == OBJ_BUTTON:
            packet.buttons.append(value[0])
        elif oid == OBJ_DIMMER:
            packet.wheel_event = value[0]
            packet.wheel_steps = value[1]
        i += 1 + length
    return packet


def input_events(packet: Packet) -> list[InputEvent]:
    """Lists the user actions a packet reports (button slots with an event, and a wheel turn)."""
    events: list[InputEvent] = []
    for slot, code in enumerate(packet.buttons, start=1):
        if code == 0:
            continue
        events.append(InputEvent(f"button:{slot}", BUTTON_EVENT_NAMES.get(code, f"event_0x{code:02x}")))
    direction = WHEEL_DIRECTION_NAMES.get(packet.wheel_event or 0)
    if direction is not None:
        events.append(InputEvent(f"wheel:{direction}", WHEEL_EVENT, packet.wheel_steps))
    return events
