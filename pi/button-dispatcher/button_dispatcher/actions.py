"""Runs the action of a fired binding: an HTTP/HTTPS request or a SwitchBot command or scene.

An action never retries. A press must not fire twice, and the user can see the result in the log
and use "Run now" to try again.
"""

from __future__ import annotations

import asyncio
import dataclasses
from typing import Any, Callable
from urllib.parse import urlsplit

import aiohttp

from .config import Config
from .switchbot import SwitchBotError, SwitchBotService

# Only the start of a response is read; the status is all that's reported.
MAX_RESPONSE_BYTES = 4096


@dataclasses.dataclass
class ActionResult:
    ok: bool
    summary: str


def host_of(url: str) -> str:
    """The scheme and host of a URL: enough for the log, without a path or query that might
    carry a key (IFTTT webhook URLs, for example, end in the user's key)."""
    parts = urlsplit(url)
    return f"{parts.scheme}://{parts.netloc}" if parts.netloc else "<url>"


def _default_content_type(body: str) -> str:
    return "application/json" if body.lstrip().startswith(("{", "[")) else "text/plain; charset=utf-8"


class ActionRunner:
    def __init__(self, get_config: Callable[[], Config],
                 get_session: Callable[[], aiohttp.ClientSession],
                 switchbot: SwitchBotService) -> None:
        self._get_config = get_config
        self._get_session = get_session
        self._switchbot = switchbot

    async def run(self, action: dict[str, Any]) -> ActionResult:
        kind = action.get("type")
        try:
            if kind == "http":
                return await self._run_http(action)
            if kind == "switchbot_command":
                client = self._switchbot.client()
                await client.command(action["device_id"], action["command"],
                                     action.get("parameter", "default"),
                                     action.get("command_type", "command"))
                return ActionResult(True, f"SwitchBot: sent {action['command']} to "
                                          f"{action.get('device_name') or action['device_id']}")
            if kind == "switchbot_scene":
                client = self._switchbot.client()
                await client.execute_scene(action["scene_id"])
                return ActionResult(True, f"SwitchBot: ran scene {action.get('scene_name') or action['scene_id']}")
        except SwitchBotError as e:
            return ActionResult(False, f"SwitchBot: {e}")
        return ActionResult(False, f"unknown action type {kind!r}")

    async def _run_http(self, action: dict[str, Any]) -> ActionResult:
        url = action["url"]
        method = action.get("method", "GET")
        timeout_s = action.get("timeout_s") or self._get_config().settings.get("http_timeout_s", 10.0)
        headers = dict(action.get("headers") or {})
        body = action.get("body") or ""
        data = body.encode("utf-8") if body else None
        if data is not None and not any(k.lower() == "content-type" for k in headers):
            headers["Content-Type"] = _default_content_type(body)
        where = host_of(url)
        try:
            async with self._get_session().request(
                    method, url, headers=headers, data=data, allow_redirects=True,
                    timeout=aiohttp.ClientTimeout(total=timeout_s)) as response:
                await response.content.read(MAX_RESPONSE_BYTES)
                ok = response.status < 400
                return ActionResult(ok, f"{method} {where}: HTTP {response.status} {response.reason or ''}".rstrip())
        except asyncio.TimeoutError:
            return ActionResult(False, f"{method} {where}: timed out after {timeout_s:g}s")
        except aiohttp.ClientError as e:
            # The exception text can contain the full URL, so only its type is reported.
            return ActionResult(False, f"{method} {where}: {type(e).__name__}")
        except ValueError:
            return ActionResult(False, f"{method} {where}: invalid request")
