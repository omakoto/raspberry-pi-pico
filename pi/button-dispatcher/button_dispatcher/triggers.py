"""Turns BTHome advertisements into fired bindings: dedupe, event extraction, matching, cooldown.

The behaviors here come from measuring real Shelly BLU remotes (see
~/history/reports/electronics/shelly-remote-switches.md):

- A sender repeats every event's advertisement 15-30 times over about a second. The packet id
  changes per event, so only the first copy of an id counts.
- A wheel turn sends a "start" packet (0 steps) at once and a "final" packet with the step count
  after the wheel stops, and reversing direction cancels the turn in progress. Firing on the start
  packet is the fastest signal; the final packet is ignored so a turn doesn't fire twice.
- Fast wiggles produce bursts of start packets, which the per-binding cooldown absorbs.
"""

from __future__ import annotations

import collections
import dataclasses
import time
from typing import Callable, Iterable, Optional

from . import bthome
from .models import Binding, Button, normalize_mac


@dataclasses.dataclass(frozen=True)
class Observed:
    """One user action heard from a device (after dedupe)."""
    mac: str
    rssi: Optional[int]
    channel: Optional[int]
    input: str
    event: str
    steps: int
    packet_id: Optional[int]
    battery: Optional[int]


@dataclasses.dataclass
class Firing:
    binding: Binding
    observed: Observed


@dataclasses.dataclass
class Result:
    """What happened to one advertisement."""
    # An identical copy of an event that was already handled.
    duplicate: bool = False
    # Why the packet could not be used at all (empty, encrypted, unsupported version).
    error: Optional[str] = None
    packet: Optional[bthome.Packet] = None
    # User actions in the packet that can trigger bindings (wheel "final" packets are left out).
    observed: list[Observed] = dataclasses.field(default_factory=list)
    fired: list[Firing] = dataclasses.field(default_factory=list)
    # Bindings that matched but didn't fire, with the reason (for example "cooldown").
    suppressed: list[tuple[Binding, str]] = dataclasses.field(default_factory=list)
    # Human-readable remarks for the event log.
    notes: list[str] = dataclasses.field(default_factory=list)
    # Whether the sending device is one of the configured buttons.
    known_device: bool = False


@dataclasses.dataclass
class _Seen:
    key: object
    time: float


def trigger_matches(binding: Binding, button: Button, obs: Observed) -> bool:
    t = binding.trigger
    return (button.mac == obs.mac and t.input == obs.input and t.event == obs.event
            and (t.channel is None or t.channel == obs.channel))


class TriggerEngine:
    # Repeats of one event arrive within a few seconds; an identical packet id seen again after
    # this long is a counter wrap or a restarted device, so it counts as a new event.
    DEDUPE_WINDOW_S = 10.0
    # Bound on remembered devices, so a flood of random addresses can't grow memory without limit.
    MAX_TRACKED_DEVICES = 256

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self._clock = clock
        self._buttons: dict[str, Button] = {}
        self._bindings: list[Binding] = []
        self._seen: collections.OrderedDict[str, _Seen] = collections.OrderedDict()
        self._last_fired: dict[str, float] = {}

    def configure(self, buttons: Iterable[Button], bindings: Iterable[Binding]) -> None:
        self._buttons = {b.id: b for b in buttons}
        self._bindings = list(bindings)
        valid = {b.id for b in self._bindings}
        self._last_fired = {k: v for k, v in self._last_fired.items() if k in valid}

    def is_known(self, mac: str) -> bool:
        return any(b.mac == mac for b in self._buttons.values())

    def process(self, mac: str, rssi: Optional[int], data: bytes) -> Result:
        """Handles one BTHome service-data payload heard from `mac`."""
        result = Result()
        try:
            mac = normalize_mac(mac)
            packet = bthome.decode(data)
        except ValueError as e:
            result.error = str(e)
            return result
        result.packet = packet
        result.known_device = self.is_known(mac)
        if packet.encrypted:
            result.error = "encrypted payload (unsupported)"
            return result
        if packet.version != 2:
            result.error = f"unsupported BTHome version {packet.version}"
            return result

        now = self._clock()
        if self._is_duplicate(mac, packet, now):
            result.duplicate = True
            return result

        for event in bthome.input_events(packet):
            if event.input.startswith("wheel:") and event.steps != 0:
                result.notes.append(f"wheel final packet ignored ({event.input}, {event.steps} steps)")
                continue
            result.observed.append(Observed(mac, rssi, packet.channel, event.input, event.event,
                                            event.steps, packet.packet_id, packet.battery))

        if result.known_device:
            for obs in result.observed:
                self._fire(obs, now, result)
        return result

    def _is_duplicate(self, mac: str, packet: bthome.Packet, now: float) -> bool:
        # Without a packet id the whole payload identifies the event.
        key: object = packet.packet_id if packet.packet_id is not None else packet.raw
        last = self._seen.get(mac)
        duplicate = last is not None and last.key == key and now - last.time <= self.DEDUPE_WINDOW_S
        # The window slides with every copy so a long burst of repeats stays one event.
        self._seen[mac] = _Seen(key, now)
        self._seen.move_to_end(mac)
        while len(self._seen) > self.MAX_TRACKED_DEVICES:
            self._seen.popitem(last=False)
        return duplicate

    def matching_bindings(self, obs: Observed) -> list[Binding]:
        """The enabled bindings an observed action matches, ignoring dedupe and cooldown."""
        matches = []
        for binding in self._bindings:
            button = self._buttons.get(binding.trigger.button)
            if binding.enabled and button is not None and trigger_matches(binding, button, obs):
                matches.append(binding)
        return matches

    def _fire(self, obs: Observed, now: float, result: Result) -> None:
        for binding in self.matching_bindings(obs):
            last = self._last_fired.get(binding.id)
            if last is not None and now - last < binding.cooldown_ms / 1000.0:
                result.suppressed.append((binding, "cooldown"))
                continue
            self._last_fired[binding.id] = now
            result.fired.append(Firing(binding, obs))
