"""A small in-memory log of what the dispatcher heard and did, shown on the web page."""

from __future__ import annotations

import collections
import time
from typing import Any, Callable


class EventLog:
    """A bounded log; every entry gets an increasing sequence number so clients can poll for news."""

    def __init__(self, capacity: int = 300, wall_clock: Callable[[], float] = time.time) -> None:
        self._entries: collections.deque[dict[str, Any]] = collections.deque(maxlen=capacity)
        self._wall_clock = wall_clock
        self._seq = 0

    def add(self, kind: str, text: str) -> None:
        """Adds an entry. `kind` is one of: ble, fire, action, error, info."""
        self._seq += 1
        self._entries.append({"seq": self._seq, "time": self._wall_clock(), "kind": kind, "text": text})

    def since(self, seq: int) -> list[dict[str, Any]]:
        return [e for e in self._entries if e["seq"] > seq]

    @property
    def last_seq(self) -> int:
        return self._seq
