"""Persistent configuration: validation, atomic storage, import and export.

The config is one JSON document (see README.md). It is validated whenever it is loaded, imported
or edited through the API, so the rest of the program can rely on it being well-formed.
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import secrets
from pathlib import Path
from typing import Any, Iterable, Optional
from urllib.parse import urlsplit

from . import bthome
from .models import Binding, Button, Trigger, normalize_mac

CONFIG_VERSION = 1

HTTP_METHODS = ("GET", "POST", "PUT", "DELETE")
ACTION_TYPES = ("http", "switchbot_command", "switchbot_scene")
SWITCHBOT_COMMAND_TYPES = ("command", "customize")
BUTTON_EVENTS = tuple(bthome.BUTTON_EVENT_NAMES.values())

DEFAULT_SETTINGS: dict[str, Any] = {"http_timeout_s": 10.0}

_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_INPUT_RE = re.compile(r"^(button:[1-9][0-9]?|wheel:(left|right))$")
_HEADER_NAME_RE = re.compile(r"^[A-Za-z0-9-]{1,64}$")
_CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")

MAX_NAME_LEN = 60
MAX_URL_LEN = 2000
MAX_BODY_LEN = 10000
MAX_HEADERS = 20
MAX_PARAMETER_JSON_LEN = 4000


class ConfigError(ValueError):
    """The config, or part of it, is invalid; the message says what and where."""


@dataclasses.dataclass
class Config:
    buttons: list[Button] = dataclasses.field(default_factory=list)
    bindings: list[Binding] = dataclasses.field(default_factory=list)
    settings: dict[str, Any] = dataclasses.field(default_factory=lambda: dict(DEFAULT_SETTINGS))
    switchbot_token: str = ""
    switchbot_secret: str = ""

    def button(self, button_id: str) -> Optional[Button]:
        return next((b for b in self.buttons if b.id == button_id), None)

    def binding(self, binding_id: str) -> Optional[Binding]:
        return next((b for b in self.bindings if b.id == binding_id), None)

    @property
    def has_switchbot_credentials(self) -> bool:
        return bool(self.switchbot_token and self.switchbot_secret)


def new_id(prefix: str) -> str:
    return f"{prefix}-{secrets.token_hex(3)}"


# ---- serialization -----------------------------------------------------------------------------

def button_to_dict(b: Button) -> dict[str, Any]:
    return {"id": b.id, "name": b.name, "mac": b.mac, "model": b.model}


def binding_to_dict(b: Binding) -> dict[str, Any]:
    t = b.trigger
    return {
        "id": b.id, "name": b.name, "enabled": b.enabled, "cooldown_ms": b.cooldown_ms,
        "trigger": {"button": t.button, "channel": t.channel, "input": t.input, "event": t.event},
        "action": b.action,
    }


def config_to_dict(config: Config, include_credentials: bool) -> dict[str, Any]:
    data: dict[str, Any] = {
        "version": CONFIG_VERSION,
        "settings": dict(config.settings),
        "buttons": [button_to_dict(b) for b in config.buttons],
        "bindings": [binding_to_dict(b) for b in config.bindings],
    }
    if include_credentials and (config.switchbot_token or config.switchbot_secret):
        data["switchbot"] = {"token": config.switchbot_token, "secret": config.switchbot_secret}
    return data


# ---- validation --------------------------------------------------------------------------------

def _require_dict(value: Any, what: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(f"{what} must be an object")
    return value


def _require_str(value: Any, what: str, max_len: int, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise ConfigError(f"{what} must be a string")
    value = value.strip()
    if not value and not allow_empty:
        raise ConfigError(f"{what} must not be empty")
    if len(value) > max_len:
        raise ConfigError(f"{what} is too long (max {max_len} characters)")
    if _CONTROL_RE.search(value):
        raise ConfigError(f"{what} must not contain control characters")
    return value


def _parse_id(value: Any, what: str) -> str:
    if not isinstance(value, str) or not _ID_RE.match(value):
        raise ConfigError(f"{what} must be 1-64 letters, digits, '-' or '_'")
    return value


def parse_button(raw: Any) -> Button:
    d = _require_dict(raw, "button")
    try:
        return Button(
            id=_parse_id(d.get("id"), "button id"),
            name=_require_str(d.get("name"), "button name", MAX_NAME_LEN),
            mac=_require_str(d.get("mac"), "button MAC address", 17),
            model=_require_str(d.get("model", ""), "button model", MAX_NAME_LEN, allow_empty=True))
    except ValueError as e:
        raise ConfigError(str(e))


def parse_trigger(raw: Any) -> Trigger:
    d = _require_dict(raw, "trigger")
    button = _parse_id(d.get("button"), "trigger button")
    trigger_input = d.get("input")
    if not isinstance(trigger_input, str) or not _INPUT_RE.match(trigger_input):
        raise ConfigError("trigger input must be button:1 .. button:99, wheel:left or wheel:right")
    event = d.get("event")
    allowed = (bthome.WHEEL_EVENT,) if trigger_input.startswith("wheel:") else BUTTON_EVENTS
    if event not in allowed:
        raise ConfigError(f"trigger event for {trigger_input} must be one of: {', '.join(allowed)}")
    channel = d.get("channel")
    if channel is not None and (isinstance(channel, bool) or not isinstance(channel, int)
                                or not 0 <= channel <= 255):
        raise ConfigError("trigger channel must be a number from 0 to 255, or empty for any channel")
    return Trigger(button=button, input=trigger_input, event=event, channel=channel)


def parse_action(raw: Any) -> dict[str, Any]:
    """Validates an action and returns a normalized copy containing only the known fields."""
    d = _require_dict(raw, "action")
    kind = d.get("type")
    if kind not in ACTION_TYPES:
        raise ConfigError(f"action type must be one of: {', '.join(ACTION_TYPES)}")

    if kind == "http":
        method = str(d.get("method", "GET")).upper()
        if method not in HTTP_METHODS:
            raise ConfigError(f"HTTP method must be one of: {', '.join(HTTP_METHODS)}")
        url = _require_str(d.get("url"), "URL", MAX_URL_LEN)
        parts = urlsplit(url)
        if parts.scheme not in ("http", "https") or not parts.hostname or " " in url:
            raise ConfigError("URL must be an http:// or https:// address")
        action: dict[str, Any] = {"type": "http", "method": method, "url": url}
        headers = d.get("headers") or {}
        if not isinstance(headers, dict) or len(headers) > MAX_HEADERS:
            raise ConfigError(f"headers must be an object with at most {MAX_HEADERS} entries")
        clean_headers: dict[str, str] = {}
        for name, value in headers.items():
            if not isinstance(name, str) or not _HEADER_NAME_RE.match(name):
                raise ConfigError(f"invalid header name: {name!r}")
            clean_headers[name] = _require_str(value, f"header {name}", 1000, allow_empty=True)
        if clean_headers:
            action["headers"] = clean_headers
        body = d.get("body") or ""
        if not isinstance(body, str) or len(body) > MAX_BODY_LEN:
            raise ConfigError(f"body must be text of at most {MAX_BODY_LEN} characters")
        if body:
            action["body"] = body
        timeout = d.get("timeout_s")
        if timeout not in (None, ""):
            if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 1 <= timeout <= 60:
                raise ConfigError("timeout must be between 1 and 60 seconds")
            action["timeout_s"] = float(timeout)
        return action

    if kind == "switchbot_command":
        action = {
            "type": kind,
            "device_id": _require_str(d.get("device_id"), "SwitchBot device", 200),
            "command": _require_str(d.get("command"), "SwitchBot command", 100),
        }
        name = _require_str(d.get("device_name", ""), "device name", 200, allow_empty=True)
        if name:
            action["device_name"] = name
        parameter = d.get("parameter", "default")
        try:
            encoded = json.dumps(parameter)
        except (TypeError, ValueError):
            raise ConfigError("SwitchBot parameter must be text, a number, or JSON")
        if len(encoded) > MAX_PARAMETER_JSON_LEN:
            raise ConfigError("SwitchBot parameter is too long")
        action["parameter"] = parameter if parameter not in (None, "") else "default"
        command_type = d.get("command_type", "command")
        if command_type not in SWITCHBOT_COMMAND_TYPES:
            raise ConfigError(f"command type must be one of: {', '.join(SWITCHBOT_COMMAND_TYPES)}")
        action["command_type"] = command_type
        return action

    action = {"type": kind, "scene_id": _require_str(d.get("scene_id"), "SwitchBot scene", 200)}
    name = _require_str(d.get("scene_name", ""), "scene name", 200, allow_empty=True)
    if name:
        action["scene_name"] = name
    return action


def parse_binding(raw: Any, button_ids: Iterable[str]) -> Binding:
    d = _require_dict(raw, "binding")
    trigger = parse_trigger(d.get("trigger"))
    if trigger.button not in set(button_ids):
        raise ConfigError(f"binding refers to an unknown button: {trigger.button!r}")
    cooldown = d.get("cooldown_ms", 700)
    if isinstance(cooldown, bool) or not isinstance(cooldown, int) or not 0 <= cooldown <= 60000:
        raise ConfigError("cooldown must be a whole number of milliseconds from 0 to 60000")
    enabled = d.get("enabled", True)
    if not isinstance(enabled, bool):
        raise ConfigError("enabled must be true or false")
    return Binding(id=_parse_id(d.get("id"), "binding id"),
                   name=_require_str(d.get("name"), "binding name", MAX_NAME_LEN),
                   trigger=trigger, action=parse_action(d.get("action")),
                   enabled=enabled, cooldown_ms=cooldown)


def parse_settings(raw: Any) -> dict[str, Any]:
    d = _require_dict(raw, "settings")
    settings = dict(DEFAULT_SETTINGS)
    timeout = d.get("http_timeout_s", settings["http_timeout_s"])
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 1 <= timeout <= 60:
        raise ConfigError("HTTP timeout must be between 1 and 60 seconds")
    settings["http_timeout_s"] = float(timeout)
    return settings


def _check_unique(buttons: list[Button], bindings: list[Binding]) -> None:
    for what, items in (("button", buttons), ("binding", bindings)):
        seen: set[str] = set()
        for item in items:
            if item.id in seen:
                raise ConfigError(f"duplicate {what} id: {item.id!r}")
            seen.add(item.id)
    macs: dict[str, str] = {}
    for b in buttons:
        if b.mac in macs:
            raise ConfigError(f"buttons {macs[b.mac]!r} and {b.id!r} have the same MAC address")
        macs[b.mac] = b.id


def config_from_dict(data: Any, extra_button_ids: Iterable[str] = ()) -> Config:
    """Validates a whole config document. Bindings may also refer to `extra_button_ids`."""
    d = _require_dict(data, "config")
    version = d.get("version", CONFIG_VERSION)
    if version != CONFIG_VERSION:
        raise ConfigError(f"unsupported config version: {version!r}")
    buttons = [parse_button(b) for b in _require_list(d.get("buttons", []), "buttons")]
    button_ids = [b.id for b in buttons] + list(extra_button_ids)
    bindings = [parse_binding(b, button_ids) for b in _require_list(d.get("bindings", []), "bindings")]
    _check_unique(buttons, bindings)
    config = Config(buttons=buttons, bindings=bindings, settings=parse_settings(d.get("settings", {})))
    switchbot = d.get("switchbot")
    if switchbot is not None:
        sb = _require_dict(switchbot, "switchbot")
        config.switchbot_token = _require_str(sb.get("token", ""), "SwitchBot token", 500, allow_empty=True)
        config.switchbot_secret = _require_str(sb.get("secret", ""), "SwitchBot secret", 500, allow_empty=True)
    return config


def _require_list(value: Any, what: str) -> list[Any]:
    if not isinstance(value, list):
        raise ConfigError(f"{what} must be a list")
    return value


# ---- import ------------------------------------------------------------------------------------

def import_config(current: Config, data: Any, mode: str) -> Config:
    """Returns `current` combined with an imported document; raises ConfigError if invalid.

    `mode` is "replace" (the document becomes the config) or "merge" (buttons and bindings with the
    same id are overwritten, others are added). Stored SwitchBot credentials are kept unless the
    document carries its own.
    """
    if mode not in ("merge", "replace"):
        raise ConfigError("import mode must be 'merge' or 'replace'")
    if mode == "replace":
        imported = config_from_dict(data)
        result = imported
    else:
        imported = config_from_dict(data, extra_button_ids=[b.id for b in current.buttons])
        buttons = {b.id: b for b in current.buttons}
        buttons.update({b.id: b for b in imported.buttons})
        bindings = {b.id: b for b in current.bindings}
        bindings.update({b.id: b for b in imported.bindings})
        settings = dict(current.settings)
        if isinstance(data, dict) and "settings" in data:
            settings = imported.settings
        result = Config(buttons=list(buttons.values()), bindings=list(bindings.values()),
                        settings=settings)
        _check_unique(result.buttons, result.bindings)
    result.switchbot_token = imported.switchbot_token or current.switchbot_token
    result.switchbot_secret = imported.switchbot_secret or current.switchbot_secret
    # A binding whose button was dropped by a replace would never fire; reject instead of keeping it.
    known = {b.id for b in result.buttons}
    for binding in result.bindings:
        if binding.trigger.button not in known:
            raise ConfigError(f"binding {binding.id!r} refers to an unknown button")
    return result


# ---- storage -----------------------------------------------------------------------------------

class ConfigStore:
    """Holds the current Config and persists it to a JSON file (mode 0600, replaced atomically)."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.config = Config()

    def load(self) -> None:
        try:
            text = self.path.read_text(encoding="utf-8")
        except FileNotFoundError:
            self.config = Config()
            return
        try:
            self.config = config_from_dict(json.loads(text))
        except (json.JSONDecodeError, ConfigError) as e:
            raise ConfigError(f"{self.path} is invalid: {e}")

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        data = json.dumps(config_to_dict(self.config, include_credentials=True), indent=2) + "\n"
        tmp = self.path.with_name(self.path.name + ".tmp")
        # Created with mode 0600 from the start, because the file holds SwitchBot credentials.
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)
