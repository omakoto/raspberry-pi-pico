"""The "Find button" flow: hear a button press, then ask for a second press to confirm it.

The first press picks a candidate; the second must come from the same device with the same
channel, control and event, which guards against picking up a neighbor's button or a stray press.
The second press has to be a separate press (a quick double press is a different event), and
repeated copies of one press never count as a second press because the trigger engine drops them
before they get here.
"""

from __future__ import annotations

import time
from typing import Any, Callable, Optional

from .triggers import Observed

EVENT_LABELS: dict[str, str] = {
    "press": "single press",
    "double_press": "double press",
    "triple_press": "triple press",
    "long_press": "long press",
    "long_double_press": "long double press",
    "long_triple_press": "long triple press",
    "hold_press": "hold",
}


def describe(obs: Observed) -> str:
    """A short human description such as "button 2, single press, channel 3"."""
    kind, _, which = obs.input.partition(":")
    if kind == "wheel":
        text = f"wheel turned {which}"
    else:
        text = f"button {which}, {EVENT_LABELS.get(obs.event, obs.event)}"
    if obs.channel is not None:
        text += f", channel {obs.channel}"
    return text


class LearnSession:
    # Presses of other devices that arrive this soon after the first one compete for the
    # candidate, and the strongest signal wins; this is what picks the button next to the Pi
    # over a neighbor's.
    COLLECT_WINDOW_S = 0.7
    FIRST_PRESS_TIMEOUT_S = 30.0
    CONFIRM_TIMEOUT_S = 30.0

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self._clock = clock
        self._reset()

    def _reset(self) -> None:
        self.phase = "idle"   # idle, waiting, confirm, confirmed, timeout
        self._deadline = 0.0
        self._collect_until: Optional[float] = None
        self._candidates: dict[str, tuple[Observed, str]] = {}
        self.candidate: Optional[Observed] = None
        self.candidate_name = ""
        self.mismatch = ""

    @property
    def active(self) -> bool:
        return self.phase in ("waiting", "confirm")

    def start(self) -> None:
        self._reset()
        self.phase = "waiting"
        self._deadline = self._clock() + self.FIRST_PRESS_TIMEOUT_S

    def cancel(self) -> None:
        self._reset()

    def feed(self, obs: Observed, name: str = "") -> None:
        """Offers a heard user action; `name` is the advertised name of the device, if known."""
        self._advance()
        if self.phase == "waiting":
            now = self._clock()
            if self._collect_until is None:
                self._collect_until = now + self.COLLECT_WINDOW_S
            self._candidates.setdefault(obs.mac, (obs, name))
        elif self.phase == "confirm" and self.candidate is not None and obs.mac == self.candidate.mac:
            same = (obs.channel, obs.input, obs.event) == (
                self.candidate.channel, self.candidate.input, self.candidate.event)
            if same:
                self.phase = "confirmed"
                self.mismatch = ""
            else:
                self.mismatch = (f"That was {describe(obs)}, but the first press was "
                                 f"{describe(self.candidate)}. Press the same button once more.")

    def _advance(self) -> None:
        now = self._clock()
        if self.phase == "waiting":
            if self._collect_until is not None and now >= self._collect_until:
                best_obs, best_name = max(self._candidates.values(),
                                          key=lambda c: c[0].rssi if c[0].rssi is not None else -127)
                self.candidate, self.candidate_name = best_obs, best_name
                self.phase = "confirm"
                # Counted from the end of the collect window rather than from now, so the
                # deadline doesn't depend on how soon someone happens to look at the session.
                self._deadline = self._collect_until + self.CONFIRM_TIMEOUT_S
            elif self._collect_until is None and now >= self._deadline:
                self.phase = "timeout"
        if self.phase == "confirm" and now >= self._deadline:
            self.phase = "timeout"

    def status(self) -> dict[str, Any]:
        self._advance()
        message = {
            "idle": "",
            "waiting": "Press the button you want to add.",
            "confirm": "Press the same button once more (a separate single press).",
            "confirmed": "Confirmed.",
            "timeout": ("No button was heard. Check the batteries and the distance to the Pi."
                        if self.candidate is None else
                        "The button wasn't confirmed in time."),
        }[self.phase]
        if self.phase == "confirm" and self.mismatch:
            message = self.mismatch
        candidate = None
        if self.candidate is not None:
            c = self.candidate
            candidate = {"mac": c.mac, "name": self.candidate_name, "rssi": c.rssi,
                         "channel": c.channel, "input": c.input, "event": c.event,
                         "battery": c.battery, "description": describe(c)}
        return {"phase": self.phase, "message": message, "candidate": candidate,
                "seconds_left": max(0.0, round(self._deadline - self._clock(), 1)) if self.active else 0.0}
