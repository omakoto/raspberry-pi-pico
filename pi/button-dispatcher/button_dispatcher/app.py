"""The web application: application state, the JSON API and the static page.

All /api routes except the sign-in ones need a valid session cookie, and every state-changing
request must also carry a custom header, which a browser won't add to a cross-site request.
"""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any, Awaitable, Callable, Optional

import aiohttp
from aiohttp import web

from . import bthome, config as cfgmod, switchbot
from .actions import ActionResult, ActionRunner
from .auth import SESSION_TTL_S, AuthStore, LoginLimiter, PasswordError
from .config import Config, ConfigError, ConfigStore, new_id
from .eventlog import EventLog
from .learn import EVENT_LABELS, LearnSession, describe
from .models import Binding, Button
from .scanner import Scanner
from .switchbot import SwitchBotError, SwitchBotService
from .triggers import Observed, TriggerEngine

COOKIE_NAME = "bd_session"
CSRF_HEADER = "X-Button-Dispatcher"
STATIC_DIR = Path(__file__).parent / "static"
PUBLIC_API = frozenset({"/api/auth/status", "/api/auth/setup", "/api/auth/login"})
SAFE_METHODS = frozenset({"GET", "HEAD", "OPTIONS"})
# Decode problems of a known device are logged at most this often per device.
ERROR_LOG_INTERVAL_S = 60.0


class ApiError(Exception):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


class AppState:
    """Everything the program knows and does, independent of HTTP."""

    def __init__(self, data_dir: Path, clock: Callable[[], float] = time.monotonic,
                 wall_clock: Callable[[], float] = time.time,
                 switchbot_api_base: str = switchbot.API_BASE) -> None:
        data_dir.mkdir(parents=True, exist_ok=True)
        self.store = ConfigStore(data_dir / "config.json")
        self.store.load()
        self.auth = AuthStore(data_dir / "auth.json", clock=wall_clock)
        self.auth.load()
        self.limiter = LoginLimiter(clock)
        self.log = EventLog(wall_clock=wall_clock)
        self.engine = TriggerEngine(clock)
        self.learn = LearnSession(clock)
        self.clock = clock
        # Advertised names by MAC; replaced by the scanner's own dict when scanning is on.
        self.names: Any = {}
        self._session: Optional[aiohttp.ClientSession] = None
        self.switchbot = SwitchBotService(
            lambda: (self.config.switchbot_token, self.config.switchbot_secret),
            self.get_session, api_base=switchbot_api_base)
        self.actions = ActionRunner(lambda: self.config, self.get_session, self.switchbot)
        self._tasks: set[asyncio.Task[Any]] = set()
        self._last_error_log: dict[str, float] = {}
        self.reconfigure()

    @property
    def config(self) -> Config:
        return self.store.config

    def get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session

    def reconfigure(self) -> None:
        self.engine.configure(self.config.buttons, self.config.bindings)

    def commit(self) -> None:
        """Persists the config and applies it to the running engine."""
        self.store.save()
        self.reconfigure()

    def button_for_mac(self, mac: str) -> Optional[Button]:
        return next((b for b in self.config.buttons if b.mac == mac), None)

    # ---- advertisements ------------------------------------------------------------------------

    def handle_packet(self, mac: str, rssi: Optional[int], data: bytes) -> None:
        """Entry point for every BTHome advertisement the scanner hears."""
        result = self.engine.process(mac, rssi, data)
        if result.error and result.known_device:
            now = self.clock()
            if now - self._last_error_log.get(mac, -ERROR_LOG_INTERVAL_S) >= ERROR_LOG_INTERVAL_S:
                self._last_error_log[mac] = now
                self.log.add("error", f"{self._label(mac)}: {result.error}")
        for obs in result.observed:
            self.learn.feed(obs, self.names.get(obs.mac, ""))
            self.log.add("ble", f"{self._label(obs.mac)}: {describe(obs)}")
        for firing in result.fired:
            self.log.add("fire", f"Binding \"{firing.binding.name}\" fired")
            self.spawn_action(firing.binding)

    def _label(self, mac: str) -> str:
        button = self.button_for_mac(mac)
        if button is not None:
            return button.name
        name = self.names.get(mac, "")
        return f"unknown device {mac}" + (f" ({name})" if name else "")

    # ---- actions -------------------------------------------------------------------------------

    def spawn_action(self, binding: Binding) -> None:
        task = asyncio.get_running_loop().create_task(self.run_binding(binding))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def run_binding(self, binding: Binding) -> ActionResult:
        try:
            result = await self.actions.run(binding.action)
        except Exception as e:  # An action must never take the whole program down.
            result = ActionResult(False, f"unexpected error: {type(e).__name__}")
        self.log.add("action" if result.ok else "error", f"\"{binding.name}\": {result.summary}")
        return result

    async def close(self) -> None:
        for task in list(self._tasks):
            task.cancel()
        if self._session is not None:
            await self._session.close()


STATE_KEY: web.AppKey[AppState] = web.AppKey("state", AppState)
SCANNER_TASK_KEY: web.AppKey[asyncio.Task[Any]] = web.AppKey("scanner_task", asyncio.Task)


# ---- helpers -----------------------------------------------------------------------------------

def json_ok(data: Any = None, status: int = 200) -> web.Response:
    return web.json_response({"ok": True} if data is None else data, status=status)


async def read_json(request: web.Request) -> dict[str, Any]:
    try:
        data = await request.json()
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise ApiError(400, "the request body is not valid JSON")
    if not isinstance(data, dict):
        raise ApiError(400, "the request body must be a JSON object")
    return data


def state_of(request: web.Request) -> AppState:
    return request.app[STATE_KEY]


def set_session_cookie(response: web.StreamResponse, state: AppState) -> None:
    response.set_cookie(COOKIE_NAME, state.auth.make_token(), max_age=SESSION_TTL_S, path="/",
                        httponly=True, samesite="Strict")


# ---- middlewares -------------------------------------------------------------------------------

@web.middleware
async def error_middleware(request: web.Request, handler: Callable[..., Awaitable[web.StreamResponse]]
                           ) -> web.StreamResponse:
    try:
        return await handler(request)
    except ApiError as e:
        return web.json_response({"error": e.message}, status=e.status)
    except (ConfigError, PasswordError) as e:
        return web.json_response({"error": str(e)}, status=400)
    except SwitchBotError as e:
        return web.json_response({"error": str(e)}, status=502)


@web.middleware
async def auth_middleware(request: web.Request, handler: Callable[..., Awaitable[web.StreamResponse]]
                          ) -> web.StreamResponse:
    if request.path.startswith("/api/"):
        if request.method not in SAFE_METHODS and request.headers.get(CSRF_HEADER) != "1":
            raise ApiError(403, f"missing {CSRF_HEADER} header")
        if request.path not in PUBLIC_API:
            if not state_of(request).auth.verify_token(request.cookies.get(COOKIE_NAME)):
                raise ApiError(401, "not signed in")
    return await handler(request)


async def add_headers(request: web.Request, response: web.StreamResponse) -> None:
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "no-referrer"
    # The page uses only its own scripts and styles; nothing is loaded from elsewhere.
    response.headers["Content-Security-Policy"] = "default-src 'self'; frame-ancestors 'none'"
    response.headers["Cache-Control"] = "no-store" if request.path.startswith("/api/") else "no-cache"


# ---- auth routes -------------------------------------------------------------------------------

async def auth_status(request: web.Request) -> web.Response:
    state = state_of(request)
    return json_ok({"configured": state.auth.is_configured,
                    "authenticated": state.auth.verify_token(request.cookies.get(COOKIE_NAME))})


async def auth_setup(request: web.Request) -> web.Response:
    state = state_of(request)
    if state.auth.is_configured:
        raise ApiError(409, "a password is already set")
    state.auth.set_password(str((await read_json(request)).get("password", "")))
    state.log.add("info", "Password set")
    response = json_ok()
    set_session_cookie(response, state)
    return response


async def auth_login(request: web.Request) -> web.Response:
    state = state_of(request)
    client = request.remote or "?"
    wait = state.limiter.retry_after(client)
    if wait > 0:
        raise ApiError(429, f"too many failed attempts; try again in {int(wait) + 1} seconds")
    if not state.auth.verify_password(str((await read_json(request)).get("password", ""))):
        state.limiter.record_failure(client)
        state.log.add("error", f"Failed sign-in from {client}")
        raise ApiError(401, "wrong password")
    state.limiter.record_success(client)
    response = json_ok()
    set_session_cookie(response, state)
    return response


async def auth_logout(request: web.Request) -> web.Response:
    response = json_ok()
    response.del_cookie(COOKIE_NAME, path="/")
    return response


async def auth_password(request: web.Request) -> web.Response:
    state = state_of(request)
    body = await read_json(request)
    if not state.auth.verify_password(str(body.get("current", ""))):
        raise ApiError(403, "the current password is wrong")
    state.auth.set_password(str(body.get("new", "")))
    state.log.add("info", "Password changed")
    response = json_ok()
    set_session_cookie(response, state)   # the old session ended with the old key
    return response


# ---- state, buttons, bindings ------------------------------------------------------------------

def choices() -> dict[str, Any]:
    return {
        "button_events": [{"value": v, "label": EVENT_LABELS.get(v, v)} for v in cfgmod.BUTTON_EVENTS],
        "wheel_events": [{"value": bthome.WHEEL_EVENT, "label": "turn"}],
        "http_methods": list(cfgmod.HTTP_METHODS),
    }


async def get_state(request: web.Request) -> web.Response:
    state = state_of(request)
    config = state.config
    return json_ok({
        "buttons": [cfgmod.button_to_dict(b) for b in config.buttons],
        "bindings": [cfgmod.binding_to_dict(b) for b in config.bindings],
        "settings": config.settings,
        "switchbot": {"configured": config.has_switchbot_credentials},
        "choices": choices(),
    })


async def put_button(request: web.Request) -> web.Response:
    state = state_of(request)
    button = state.config.button(request.match_info["id"])
    if button is None:
        raise ApiError(404, "no such button")
    body = await read_json(request)
    updated = cfgmod.parse_button({"id": button.id, "name": body.get("name"), "mac": button.mac,
                                   "model": button.model})
    button.name = updated.name
    state.commit()
    return json_ok(cfgmod.button_to_dict(button))


async def delete_button(request: web.Request) -> web.Response:
    state = state_of(request)
    button = state.config.button(request.match_info["id"])
    if button is None:
        raise ApiError(404, "no such button")
    users = [b for b in state.config.bindings if b.trigger.button == button.id]
    if users and request.query.get("cascade") != "1":
        raise ApiError(409, f"{len(users)} binding(s) use this button; delete them too, or retry "
                            "with cascade=1")
    state.config.bindings = [b for b in state.config.bindings if b.trigger.button != button.id]
    state.config.buttons = [b for b in state.config.buttons if b.id != button.id]
    state.commit()
    return json_ok()


async def post_binding(request: web.Request) -> web.Response:
    state = state_of(request)
    body = await read_json(request)
    binding = cfgmod.parse_binding({**body, "id": new_id("bind")}, [b.id for b in state.config.buttons])
    state.config.bindings.append(binding)
    state.commit()
    return json_ok(cfgmod.binding_to_dict(binding), status=201)


async def put_binding(request: web.Request) -> web.Response:
    state = state_of(request)
    binding_id = request.match_info["id"]
    index = next((i for i, b in enumerate(state.config.bindings) if b.id == binding_id), None)
    if index is None:
        raise ApiError(404, "no such binding")
    body = await read_json(request)
    binding = cfgmod.parse_binding({**body, "id": binding_id}, [b.id for b in state.config.buttons])
    state.config.bindings[index] = binding
    state.commit()
    return json_ok(cfgmod.binding_to_dict(binding))


async def delete_binding(request: web.Request) -> web.Response:
    state = state_of(request)
    binding_id = request.match_info["id"]
    if state.config.binding(binding_id) is None:
        raise ApiError(404, "no such binding")
    state.config.bindings = [b for b in state.config.bindings if b.id != binding_id]
    state.commit()
    return json_ok()


async def run_binding_now(request: web.Request) -> web.Response:
    state = state_of(request)
    binding = state.config.binding(request.match_info["id"])
    if binding is None:
        raise ApiError(404, "no such binding")
    result = await state.run_binding(binding)
    return json_ok({"ok": result.ok, "summary": result.summary})


async def simulate(request: web.Request) -> web.Response:
    """Fires the bindings of a trigger as if the button had been pressed (cooldown is ignored)."""
    state = state_of(request)
    body = await read_json(request)
    trigger = cfgmod.parse_trigger(body)
    button = state.config.button(trigger.button)
    if button is None:
        raise ApiError(404, "no such button")
    obs = Observed(button.mac, None, trigger.channel, trigger.input, trigger.event, 0, None, None)
    matched = state.engine.matching_bindings(obs)
    state.log.add("info", f"Simulated: {button.name}, {describe(obs)} -> {len(matched)} binding(s)")
    for binding in matched:
        state.spawn_action(binding)
    return json_ok({"matched": [b.name for b in matched]})


# ---- learn, log, settings ----------------------------------------------------------------------

async def learn_start(request: web.Request) -> web.Response:
    state_of(request).learn.start()
    return json_ok(state_of(request).learn.status())


async def learn_status(request: web.Request) -> web.Response:
    state = state_of(request)
    status = state.learn.status()
    candidate = status["candidate"]
    if candidate is not None:
        existing = state.button_for_mac(candidate["mac"])
        candidate["known_button"] = existing.id if existing else None
    return json_ok(status)


async def learn_cancel(request: web.Request) -> web.Response:
    state_of(request).learn.cancel()
    return json_ok()


async def learn_save(request: web.Request) -> web.Response:
    state = state_of(request)
    body = await read_json(request)
    status = state.learn.status()
    if status["phase"] != "confirmed" or state.learn.candidate is None:
        raise ApiError(409, "no confirmed button to save; use Find button first")
    candidate = state.learn.candidate
    button = state.button_for_mac(candidate.mac)
    created = button is None
    if button is None:
        button = cfgmod.parse_button({"id": new_id("btn"), "name": body.get("name"),
                                      "mac": candidate.mac, "model": state.learn.candidate_name})
        state.config.buttons.append(button)
        state.commit()
        state.log.add("info", f"Added button \"{button.name}\" ({button.mac})")
    state.learn.cancel()
    return json_ok({"button": cfgmod.button_to_dict(button), "created": created,
                    "trigger": {"button": button.id, "channel": candidate.channel,
                                "input": candidate.input, "event": candidate.event}},
                   status=201 if created else 200)


async def get_log(request: web.Request) -> web.Response:
    state = state_of(request)
    try:
        since = int(request.query.get("since", "0"))
    except ValueError:
        raise ApiError(400, "since must be a number")
    return json_ok({"entries": state.log.since(since), "last_seq": state.log.last_seq})


async def put_settings(request: web.Request) -> web.Response:
    state = state_of(request)
    state.config.settings = cfgmod.parse_settings({**state.config.settings, **await read_json(request)})
    state.commit()
    return json_ok(state.config.settings)


# ---- SwitchBot ---------------------------------------------------------------------------------

async def put_switchbot_credentials(request: web.Request) -> web.Response:
    """Sets the SwitchBot token and/or secret; a field left out or empty keeps its stored value."""
    state = state_of(request)
    body = await read_json(request)
    token, secret = body.get("token", ""), body.get("secret", "")
    if not isinstance(token, str) or not isinstance(secret, str):
        raise ApiError(400, "token and secret must be text")
    if token.strip():
        state.config.switchbot_token = token.strip()
    if secret.strip():
        state.config.switchbot_secret = secret.strip()
    state.commit()
    state.switchbot.invalidate()
    return json_ok({"configured": state.config.has_switchbot_credentials})


async def delete_switchbot_credentials(request: web.Request) -> web.Response:
    state = state_of(request)
    state.config.switchbot_token = state.config.switchbot_secret = ""
    state.commit()
    state.switchbot.invalidate()
    return json_ok({"configured": False})


async def switchbot_devices(request: web.Request) -> web.Response:
    return json_ok({"devices": await state_of(request).switchbot.devices(request.query.get("refresh") == "1")})


async def switchbot_scenes(request: web.Request) -> web.Response:
    return json_ok({"scenes": await state_of(request).switchbot.scenes(request.query.get("refresh") == "1")})


async def switchbot_commands(request: web.Request) -> web.Response:
    device_id = request.query.get("device_id", "")
    if not device_id:
        raise ApiError(400, "device_id is required")
    return json_ok(await state_of(request).switchbot.commands_for(device_id))


# ---- export / import ---------------------------------------------------------------------------

async def export_config(request: web.Request) -> web.Response:
    state = state_of(request)
    include = request.query.get("credentials") == "1"
    text = json.dumps(cfgmod.config_to_dict(state.config, include_credentials=include), indent=2) + "\n"
    return web.Response(text=text, content_type="application/json",
                        headers={"Content-Disposition": 'attachment; filename="button-dispatcher-config.json"'})


async def import_config(request: web.Request) -> web.Response:
    state = state_of(request)
    body = await read_json(request)
    if "config" not in body:
        raise ApiError(400, "the request needs a 'config' field")
    state.store.config = cfgmod.import_config(state.config, body["config"], str(body.get("mode", "merge")))
    state.commit()
    state.switchbot.invalidate()
    state.log.add("info", f"Imported config ({body.get('mode', 'merge')}): "
                          f"{len(state.config.buttons)} button(s), {len(state.config.bindings)} binding(s)")
    return json_ok({"buttons": len(state.config.buttons), "bindings": len(state.config.bindings)})


# ---- static page -------------------------------------------------------------------------------

async def index(request: web.Request) -> web.StreamResponse:
    page = STATIC_DIR / "index.html"
    if not page.exists():
        raise ApiError(404, "the web page is missing from this installation")
    return web.FileResponse(page)


# ---- application -------------------------------------------------------------------------------

def create_app(data_dir: Path, ble: bool = True, state: Optional[AppState] = None) -> web.Application:
    """Builds the web application; `ble` turns the Bluetooth scanner on."""
    app = web.Application(middlewares=[error_middleware, auth_middleware])
    app[STATE_KEY] = state or AppState(data_dir)
    app.on_response_prepare.append(add_headers)

    app.router.add_get("/", index)
    app.router.add_static("/static", STATIC_DIR, show_index=False) if STATIC_DIR.exists() else None
    r = app.router
    r.add_get("/api/auth/status", auth_status)
    r.add_post("/api/auth/setup", auth_setup)
    r.add_post("/api/auth/login", auth_login)
    r.add_post("/api/auth/logout", auth_logout)
    r.add_post("/api/auth/password", auth_password)
    r.add_get("/api/state", get_state)
    r.add_put("/api/buttons/{id}", put_button)
    r.add_delete("/api/buttons/{id}", delete_button)
    r.add_post("/api/bindings", post_binding)
    r.add_put("/api/bindings/{id}", put_binding)
    r.add_delete("/api/bindings/{id}", delete_binding)
    r.add_post("/api/bindings/{id}/run", run_binding_now)
    r.add_post("/api/simulate", simulate)
    r.add_post("/api/learn/start", learn_start)
    r.add_get("/api/learn", learn_status)
    r.add_post("/api/learn/cancel", learn_cancel)
    r.add_post("/api/learn/save", learn_save)
    r.add_get("/api/log", get_log)
    r.add_put("/api/settings", put_settings)
    r.add_put("/api/switchbot/credentials", put_switchbot_credentials)
    r.add_delete("/api/switchbot/credentials", delete_switchbot_credentials)
    r.add_get("/api/switchbot/devices", switchbot_devices)
    r.add_get("/api/switchbot/scenes", switchbot_scenes)
    r.add_get("/api/switchbot/commands", switchbot_commands)
    r.add_get("/api/export", export_config)
    r.add_post("/api/import", import_config)

    async def start_scanner(app: web.Application) -> None:
        if not ble:
            return
        st = app[STATE_KEY]
        scanner = Scanner(st.handle_packet, log=st.log.add)
        st.names = scanner.names
        app[SCANNER_TASK_KEY] = asyncio.get_running_loop().create_task(scanner.run())

    async def stop_everything(app: web.Application) -> None:
        task = app.get(SCANNER_TASK_KEY)
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        await app[STATE_KEY].close()

    app.on_startup.append(start_scanner)
    app.on_cleanup.append(stop_everything)
    return app
