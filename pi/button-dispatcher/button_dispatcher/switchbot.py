"""SwitchBot Open API v1.1 client and a small service that caches the device and scene lists.

API reference: https://github.com/OpenWonderLabs/SwitchBotAPI
Every request carries the token, a timestamp, a nonce and an HMAC-SHA256 signature of the three
made with the secret. Credentials come from the config (typed in the web UI), never from code.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import time
import uuid
from typing import Any, Callable, Optional

import aiohttp

from . import switchbot_commands

API_BASE = "https://api.switch-bot.com/v1.1"
TIMEOUT_S = 15.0

# statusCode values in the response body; anything but STATUS_OK is an error even on HTTP 200.
STATUS_OK = 100
STATUS_MESSAGES: dict[int, str] = {
    151: "device type error",
    152: "device not found",
    160: "command is not supported by this device",
    161: "device offline",
    171: "hub device offline",
    190: "device internal error (often an invalid parameter or command)",
}


class SwitchBotError(Exception):
    pass


def make_auth_headers(token: str, secret: str, t: Optional[str] = None,
                      nonce: Optional[str] = None) -> dict[str, str]:
    """Builds the signed headers the API requires on every request."""
    t = t if t is not None else str(int(time.time() * 1000))
    nonce = nonce if nonce is not None else str(uuid.uuid4())
    digest = hmac.new(secret.encode(), (token + t + nonce).encode(), hashlib.sha256).digest()
    return {
        "Authorization": token,
        "t": t,
        "nonce": nonce,
        "sign": base64.b64encode(digest).decode(),
        "Content-Type": "application/json; charset=utf8",
    }


class SwitchBotClient:
    def __init__(self, token: str, secret: str, session: aiohttp.ClientSession,
                 api_base: str = API_BASE, timeout_s: float = TIMEOUT_S) -> None:
        self._token = token
        self._secret = secret
        self._session = session
        self._api_base = api_base
        self._timeout = aiohttp.ClientTimeout(total=timeout_s)

    async def request(self, method: str, path: str, body: Optional[dict[str, Any]] = None) -> Any:
        """Sends a request and returns the response's `body` field."""
        url = self._api_base + path
        data = json.dumps(body) if body is not None else None
        headers = make_auth_headers(self._token, self._secret)
        try:
            async with self._session.request(method, url, data=data, headers=headers,
                                             timeout=self._timeout) as response:
                status = response.status
                raw = await response.read()
        except asyncio.TimeoutError:
            raise SwitchBotError(f"timed out after {self._timeout.total:g}s")
        except aiohttp.ClientError as e:
            raise SwitchBotError(f"cannot reach SwitchBot: {type(e).__name__}")

        if status == 401:
            raise SwitchBotError("HTTP 401: authentication failed; check the token and secret")
        if status >= 400:
            raise SwitchBotError(f"HTTP {status}")
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            raise SwitchBotError("invalid response from SwitchBot")
        code = payload.get("statusCode")
        if code != STATUS_OK:
            message = payload.get("message") or STATUS_MESSAGES.get(code, "unknown error")
            hint = STATUS_MESSAGES.get(code)
            if hint and hint != message:
                message = f"{message} ({hint})"
            raise SwitchBotError(f"API error {code}: {message}")
        return payload.get("body")

    async def devices(self) -> dict[str, Any]:
        return await self.request("GET", "/devices")

    async def scenes(self) -> list[dict[str, Any]]:
        return await self.request("GET", "/scenes")

    async def command(self, device_id: str, command: str, parameter: Any = "default",
                      command_type: str = "command") -> Any:
        return await self.request("POST", f"/devices/{device_id}/commands",
                                  {"command": command, "parameter": parameter,
                                   "commandType": command_type})

    async def execute_scene(self, scene_id: str) -> Any:
        return await self.request("POST", f"/scenes/{scene_id}/execute")


def device_items(body: Optional[dict[str, Any]]) -> list[dict[str, Any]]:
    """Reduces the GET /devices body to what the UI lists: physical devices and IR remotes."""
    body = body or {}
    items = [{"id": d["deviceId"], "name": d.get("deviceName", ""), "type": d.get("deviceType", ""),
              "hub": d.get("hubDeviceId", ""), "remote_type": None}
             for d in body.get("deviceList") or []]
    items += [{"id": d["deviceId"], "name": d.get("deviceName", ""),
               "type": f"IR {d.get('remoteType', '')}".strip(), "hub": d.get("hubDeviceId", ""),
               "remote_type": d.get("remoteType", "")}
              for d in body.get("infraredRemoteList") or []]
    return items


def scene_items(body: Optional[list[dict[str, Any]]]) -> list[dict[str, Any]]:
    return [{"id": s["sceneId"], "name": s.get("sceneName", "")} for s in body or []]


class SwitchBotService:
    """Creates clients from the current credentials and caches the device and scene lists."""

    CACHE_TTL_S = 300.0

    def __init__(self, get_credentials: Callable[[], tuple[str, str]],
                 get_session: Callable[[], aiohttp.ClientSession], api_base: str = API_BASE,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._get_credentials = get_credentials
        self._get_session = get_session
        self._api_base = api_base
        self._clock = clock
        self._cache: dict[str, tuple[float, list[dict[str, Any]]]] = {}

    def client(self) -> SwitchBotClient:
        token, secret = self._get_credentials()
        if not token or not secret:
            raise SwitchBotError("SwitchBot token and secret are not set (see Settings)")
        return SwitchBotClient(token, secret, self._get_session(), self._api_base)

    def invalidate(self) -> None:
        """Drops cached lists, for example after the credentials changed."""
        self._cache.clear()

    async def devices(self, refresh: bool = False) -> list[dict[str, Any]]:
        return await self._cached("devices", refresh, lambda c: c.devices(), device_items)

    async def scenes(self, refresh: bool = False) -> list[dict[str, Any]]:
        return await self._cached("scenes", refresh, lambda c: c.scenes(), scene_items)

    async def _cached(self, key: str, refresh: bool, fetch: Callable[[SwitchBotClient], Any],
                      convert: Callable[[Any], list[dict[str, Any]]]) -> list[dict[str, Any]]:
        entry = self._cache.get(key)
        if entry is not None and not refresh and self._clock() - entry[0] < self.CACHE_TTL_S:
            return entry[1]
        items = convert(await fetch(self.client()))
        self._cache[key] = (self._clock(), items)
        return items

    async def commands_for(self, device_id: str) -> dict[str, Any]:
        """The documented commands for one of the user's devices (looked up in the device list)."""
        for device in await self.devices():
            if device["id"] == device_id:
                rows, notes = switchbot_commands.commands_for(device["type"], device["remote_type"])
                return {"device": device, "notes": notes,
                        "commands": [{"command": c, "parameter": p, "description": d}
                                     for c, p, d in rows]}
        raise SwitchBotError(f"no device {device_id!r} in your SwitchBot account; refresh the list")
