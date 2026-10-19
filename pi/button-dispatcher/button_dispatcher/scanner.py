"""BLE scanning: listens for BTHome advertisements and passes them to a callback.

Uses bleak (BlueZ on the Pi). The scanner runs for as long as the program does, so it recovers
from adapter errors and from BlueZ silently stopping delivery: either one restarts the scan after
a pause.
"""

from __future__ import annotations

import asyncio
import collections
import time
from typing import Any, Callable, Optional

from . import bthome

# Called with (MAC address, RSSI, BTHome service data) for each BTHome advertisement.
PacketCallback = Callable[[str, Optional[int], bytes], None]
LogCallback = Callable[[str, str], None]


class Scanner:
    # With no advertisements at all for this long, the scan is assumed stalled and restarted.
    STALL_S = 120.0
    MAX_BACKOFF_S = 30.0
    MAX_NAMES = 1024

    def __init__(self, on_packet: PacketCallback, log: Optional[LogCallback] = None,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._on_packet = on_packet
        self._log = log or (lambda kind, text: None)
        self._clock = clock
        self._last_advert = clock()
        # Advertised device names by MAC. The name is in a different advertisement from the
        # BTHome data, so it is remembered from any advertisement that carries one.
        self.names: collections.OrderedDict[str, str] = collections.OrderedDict()

    def _remember_name(self, mac: str, name: str) -> None:
        self.names[mac] = name
        self.names.move_to_end(mac)
        while len(self.names) > self.MAX_NAMES:
            self.names.popitem(last=False)

    def _on_advert(self, device: Any, advert: Any) -> None:
        """bleak's detection callback; runs on the event loop, so it must not block."""
        self._last_advert = self._clock()
        if advert.local_name:
            self._remember_name(device.address.upper(), advert.local_name)
        data = advert.service_data.get(bthome.SERVICE_UUID)
        if data is not None:
            self._on_packet(device.address.upper(), advert.rssi, bytes(data))

    async def run(self) -> None:
        # Imported here so that everything else (and the tests) work without Bluetooth.
        from bleak import BleakScanner

        backoff = 1.0
        while True:
            try:
                async with BleakScanner(self._on_advert):
                    self._last_advert = self._clock()
                    self._log("info", "Bluetooth scanning started")
                    backoff = 1.0
                    while True:
                        await asyncio.sleep(5)
                        if self._clock() - self._last_advert > self.STALL_S:
                            raise TimeoutError(f"no advertisements for {self.STALL_S:g}s")
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self._log("error", f"Bluetooth scan stopped ({type(e).__name__}: {e}); "
                                   f"restarting in {backoff:g}s")
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, self.MAX_BACKOFF_S)
