"""Plain data types shared by the config, the trigger engine and the web API."""

from __future__ import annotations

import dataclasses
import re
from typing import Any, Optional

_MAC_RE = re.compile(r"^[0-9A-F]{2}([:-]?[0-9A-F]{2}){5}$")


def normalize_mac(value: str) -> str:
    """Returns a MAC address as upper-case `AA:BB:CC:DD:EE:FF`; raises ValueError if invalid."""
    text = value.strip().upper()
    if not _MAC_RE.match(text):
        raise ValueError(f"invalid MAC address: {value!r}")
    digits = text.replace(":", "").replace("-", "")
    return ":".join(digits[i:i + 2] for i in range(0, 12, 2))


@dataclasses.dataclass
class Button:
    """A BLE button or remote the user has taught the dispatcher."""
    id: str
    name: str
    mac: str
    # The advertised model code (for example "SBBT-104CUS"); shown to the user, never matched on.
    model: str = ""

    def __post_init__(self) -> None:
        self.mac = normalize_mac(self.mac)


@dataclasses.dataclass
class Trigger:
    """Which user action fires a binding."""
    button: str
    # "button:N" (N from 1) or "wheel:left" / "wheel:right".
    input: str
    event: str
    # Match only this channel (0-3 on the Shelly Remote Control), or any channel when None.
    channel: Optional[int] = None


@dataclasses.dataclass
class Binding:
    """A trigger plus the action to run when it fires."""
    id: str
    name: str
    trigger: Trigger
    # The action description (see actions.py); opaque to the trigger engine.
    action: dict[str, Any]
    enabled: bool = True
    # Minimum time between two firings of this binding; absorbs wheel wiggles.
    cooldown_ms: int = 700
