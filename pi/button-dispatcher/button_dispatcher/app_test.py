"""API tests for app.py: the real application driven over HTTP, with fake HTTP and SwitchBot servers.

Run from the project directory: python3 -m unittest discover -p '*_test.py'
Needs aiohttp (the tests are skipped without it).
"""

import asyncio
import json
import tempfile
import unittest
from pathlib import Path
from typing import Any, Optional

try:
    from aiohttp import web
    from aiohttp.test_utils import TestClient, TestServer
except ImportError:
    raise unittest.SkipTest("aiohttp is not installed")

from button_dispatcher.app import CSRF_HEADER, AppState, create_app
from button_dispatcher.auth import LoginLimiter
from button_dispatcher.switchbot_test import SECRET, TOKEN, FakeSwitchBot

DESK = "AA:BB:CC:DD:EE:01"
PRESS_B2 = "44002101643a003a013a003a00"       # id 33, button 2 press (four-button remote)
PRESS_B2_AGAIN = "44002201643a003a013a003a00"  # id 34, the same press again
PRESS_B1 = "44002301643a013a003a003a00"       # id 35, button 1 press
PASSWORD = "correct horse battery"


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


class ApiTestCase(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.data_dir = Path(self.tmp.name)
        self.clock = FakeClock()
        self.hits: list[dict[str, Any]] = []
        target = web.Application()
        target.router.add_route("*", "/hit", self.hit)
        self.target = TestServer(target)
        await self.target.start_server()
        self.fake_sb = FakeSwitchBot()
        await self.fake_sb.server.start_server()
        await self.open_app()

    async def asyncTearDown(self) -> None:
        await self.client.close()
        await self.target.close()
        await self.fake_sb.server.close()
        self.tmp.cleanup()

    async def open_app(self) -> None:
        self.state = AppState(self.data_dir, clock=self.clock, wall_clock=self.clock,
                              switchbot_api_base=self.fake_sb.api_base)
        self.client = TestClient(TestServer(create_app(self.data_dir, ble=False, state=self.state)))
        await self.client.start_server()

    async def hit(self, request: web.Request) -> web.Response:
        self.hits.append({"method": request.method, "path": request.path_qs})
        return web.Response(text="ok")

    def target_url(self, path: str = "/hit") -> str:
        return str(self.target.make_url(path))

    async def api(self, method: str, path: str, body: Optional[dict] = None, csrf: bool = True):
        headers = {CSRF_HEADER: "1"} if csrf else {}
        response = await self.client.request(method, path, json=body, headers=headers)
        try:
            data = await response.json()
        except Exception:
            data = None
        return response.status, data

    async def sign_in(self) -> None:
        status, _ = await self.api("POST", "/api/auth/setup", {"password": PASSWORD})
        self.assertEqual(status, 200)

    async def add_button(self, name: str = "Desk remote") -> dict:
        """Adds DESK through the real "Find button" flow."""
        await self.api("POST", "/api/learn/start")
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2))
        self.clock.advance(1)
        status, data = await self.api("GET", "/api/learn")
        self.assertEqual(data["phase"], "confirm")
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2_AGAIN))
        status, data = await self.api("POST", "/api/learn/save", {"name": name})
        self.assertIn(status, (200, 201), data)
        return data

    def http_binding(self, button_id: str, **overrides: Any) -> dict:
        binding = {"name": "Hit target", "enabled": True, "cooldown_ms": 0,
                   "trigger": {"button": button_id, "channel": None, "input": "button:2", "event": "press"},
                   "action": {"type": "http", "method": "GET", "url": self.target_url()}}
        binding.update(overrides)
        return binding

    async def settle(self) -> None:
        """Waits for the actions started by presses to finish."""
        while self.state._tasks:
            await asyncio.gather(*list(self.state._tasks))


class AuthTest(ApiTestCase):
    async def test_status_before_and_after_setup(self) -> None:
        self.assertEqual(await self.api("GET", "/api/auth/status"),
                         (200, {"configured": False, "authenticated": False}))
        await self.sign_in()
        self.assertEqual(await self.api("GET", "/api/auth/status"),
                         (200, {"configured": True, "authenticated": True}))

    async def test_setup_rejects_short_passwords_and_runs_only_once(self) -> None:
        status, data = await self.api("POST", "/api/auth/setup", {"password": "short"})
        self.assertEqual(status, 400)
        self.assertIn("password", data["error"])
        await self.sign_in()
        status, _ = await self.api("POST", "/api/auth/setup", {"password": "another password"})
        self.assertEqual(status, 409)

    async def test_api_needs_a_session(self) -> None:
        self.assertEqual((await self.api("GET", "/api/state"))[0], 401)
        await self.sign_in()
        self.assertEqual((await self.api("GET", "/api/state"))[0], 200)

    async def test_changes_need_the_csrf_header(self) -> None:
        await self.sign_in()
        status, data = await self.api("POST", "/api/learn/start", csrf=False)
        self.assertEqual(status, 403)
        self.assertIn(CSRF_HEADER, data["error"])
        # Sign-in endpoints are public but still need the header.
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": PASSWORD}, csrf=False))[0], 403)
        # Reads don't.
        self.assertEqual((await self.api("GET", "/api/state", csrf=False))[0], 200)

    async def test_login_logout(self) -> None:
        await self.sign_in()
        await self.api("POST", "/api/auth/logout")
        self.assertEqual((await self.api("GET", "/api/state"))[0], 401)
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": "wrong wrong"}))[0], 401)
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": PASSWORD}))[0], 200)
        self.assertEqual((await self.api("GET", "/api/state"))[0], 200)

    async def test_repeated_wrong_passwords_are_throttled(self) -> None:
        await self.sign_in()
        await self.api("POST", "/api/auth/logout")
        for _ in range(LoginLimiter.MAX_FAILURES):
            self.assertEqual((await self.api("POST", "/api/auth/login", {"password": "nope nope"}))[0], 401)
        status, data = await self.api("POST", "/api/auth/login", {"password": PASSWORD})
        self.assertEqual(status, 429)
        self.assertIn("try again", data["error"])
        self.clock.advance(LoginLimiter.BLOCK_S + 1)
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": PASSWORD}))[0], 200)

    async def test_change_password(self) -> None:
        await self.sign_in()
        status, _ = await self.api("POST", "/api/auth/password", {"current": "wrong wrong", "new": "brand new pass"})
        self.assertEqual(status, 403)
        status, _ = await self.api("POST", "/api/auth/password", {"current": PASSWORD, "new": "brand new pass"})
        self.assertEqual(status, 200)
        self.assertEqual((await self.api("GET", "/api/state"))[0], 200)   # still signed in
        await self.api("POST", "/api/auth/logout")
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": PASSWORD}))[0], 401)
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": "brand new pass"}))[0], 200)

    async def test_security_headers(self) -> None:
        response = await self.client.get("/api/auth/status")
        self.assertEqual(response.headers["X-Frame-Options"], "DENY")
        self.assertEqual(response.headers["X-Content-Type-Options"], "nosniff")
        self.assertIn("default-src 'self'", response.headers["Content-Security-Policy"])
        self.assertEqual(response.headers["Cache-Control"], "no-store")

    async def test_session_cookie_is_http_only_and_strict(self) -> None:
        response = await self.client.post("/api/auth/setup", json={"password": PASSWORD},
                                          headers={CSRF_HEADER: "1"})
        cookie = response.headers["Set-Cookie"]
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Strict", cookie)


class LearnTest(ApiTestCase):
    async def test_find_button_flow_adds_the_button(self) -> None:
        await self.sign_in()
        data = await self.add_button("Desk remote")
        self.assertTrue(data["created"])
        self.assertEqual(data["button"]["mac"], DESK)
        self.assertEqual(data["trigger"], {"button": data["button"]["id"], "channel": None,
                                           "input": "button:2", "event": "press"})
        _, state = await self.api("GET", "/api/state")
        self.assertEqual([b["name"] for b in state["buttons"]], ["Desk remote"])
        # Saved to disk: a fresh application over the same data directory still has it.
        await self.client.close()
        await self.open_app()
        self.assertEqual(self.state.config.buttons[0].mac, DESK)

    async def test_learning_an_already_known_button_reuses_it(self) -> None:
        await self.sign_in()
        first = await self.add_button("Desk remote")
        second = await self.add_button("Different name")
        self.assertFalse(second["created"])
        self.assertEqual(second["button"]["id"], first["button"]["id"])
        _, state = await self.api("GET", "/api/state")
        self.assertEqual(len(state["buttons"]), 1)

    async def test_status_names_the_known_button(self) -> None:
        await self.sign_in()
        await self.add_button()
        await self.api("POST", "/api/learn/start")
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B1))
        self.clock.advance(1)
        _, status = await self.api("GET", "/api/learn")
        self.assertTrue(status["candidate"]["known_button"])

    async def test_save_without_a_confirmed_press_is_refused(self) -> None:
        await self.sign_in()
        await self.api("POST", "/api/learn/start")
        status, _ = await self.api("POST", "/api/learn/save", {"name": "Nope"})
        self.assertEqual(status, 409)

    async def test_cancel(self) -> None:
        await self.sign_in()
        await self.api("POST", "/api/learn/start")
        await self.api("POST", "/api/learn/cancel")
        self.assertEqual((await self.api("GET", "/api/learn"))[1]["phase"], "idle")

    async def test_advertised_name_becomes_the_model(self) -> None:
        await self.sign_in()
        self.state.names = {DESK: "SBBT-104CUS"}
        data = await self.add_button()
        self.assertEqual(data["button"]["model"], "SBBT-104CUS")


class ButtonAndBindingTest(ApiTestCase):
    async def asyncSetUp(self) -> None:
        await super().asyncSetUp()
        await self.sign_in()
        self.button = (await self.add_button())["button"]

    async def test_state_lists_buttons_bindings_and_choices(self) -> None:
        _, state = await self.api("GET", "/api/state")
        self.assertEqual(state["bindings"], [])
        self.assertEqual(state["switchbot"], {"configured": False})
        self.assertIn("press", [e["value"] for e in state["choices"]["button_events"]])
        self.assertEqual(state["choices"]["wheel_events"][0]["value"], "turn")

    async def test_binding_crud(self) -> None:
        status, created = await self.api("POST", "/api/bindings", self.http_binding(self.button["id"]))
        self.assertEqual(status, 201)
        binding_id = created["id"]
        edited = self.http_binding(self.button["id"], name="Renamed", enabled=False)
        status, data = await self.api("PUT", f"/api/bindings/{binding_id}", edited)
        self.assertEqual((status, data["name"], data["enabled"]), (200, "Renamed", False))
        _, state = await self.api("GET", "/api/state")
        self.assertEqual([b["name"] for b in state["bindings"]], ["Renamed"])
        self.assertEqual((await self.api("DELETE", f"/api/bindings/{binding_id}"))[0], 200)
        self.assertEqual((await self.api("DELETE", f"/api/bindings/{binding_id}"))[0], 404)
        self.assertEqual((await self.api("PUT", f"/api/bindings/{binding_id}", edited))[0], 404)

    async def test_invalid_bindings_are_explained(self) -> None:
        status, data = await self.api("POST", "/api/bindings", self.http_binding(
            self.button["id"], action={"type": "http", "url": "ftp://nope"}))
        self.assertEqual(status, 400)
        self.assertIn("URL", data["error"])
        status, data = await self.api("POST", "/api/bindings", self.http_binding("ghost"))
        self.assertEqual(status, 400)
        self.assertIn("unknown button", data["error"])
        self.assertEqual((await self.api("POST", "/api/bindings", {"nonsense": True}))[0], 400)
        _, state = await self.api("GET", "/api/state")
        self.assertEqual(state["bindings"], [])

    async def test_bad_json_body(self) -> None:
        response = await self.client.post("/api/bindings", data="{oops", headers={CSRF_HEADER: "1"})
        self.assertEqual(response.status, 400)
        response = await self.client.post("/api/bindings", data="[1]", headers={CSRF_HEADER: "1"})
        self.assertEqual(response.status, 400)

    async def test_rename_button(self) -> None:
        status, data = await self.api("PUT", f"/api/buttons/{self.button['id']}", {"name": "Bedside"})
        self.assertEqual((status, data["name"], data["mac"]), (200, "Bedside", DESK))
        self.assertEqual((await self.api("PUT", f"/api/buttons/{self.button['id']}", {"name": ""}))[0], 400)
        self.assertEqual((await self.api("PUT", "/api/buttons/ghost", {"name": "x"}))[0], 404)

    async def test_deleting_a_used_button_needs_cascade(self) -> None:
        await self.api("POST", "/api/bindings", self.http_binding(self.button["id"]))
        status, data = await self.api("DELETE", f"/api/buttons/{self.button['id']}")
        self.assertEqual(status, 409)
        self.assertIn("cascade", data["error"])
        self.assertEqual((await self.api("DELETE", f"/api/buttons/{self.button['id']}?cascade=1"))[0], 200)
        _, state = await self.api("GET", "/api/state")
        self.assertEqual((state["buttons"], state["bindings"]), ([], []))

    async def test_deleting_an_unused_button(self) -> None:
        self.assertEqual((await self.api("DELETE", f"/api/buttons/{self.button['id']}"))[0], 200)
        self.assertEqual((await self.api("DELETE", f"/api/buttons/{self.button['id']}"))[0], 404)


class FiringTest(ApiTestCase):
    async def asyncSetUp(self) -> None:
        await super().asyncSetUp()
        await self.sign_in()
        self.button = (await self.add_button())["button"]
        self.clock.advance(60)   # the presses used to add the button are long past

    async def add_binding(self, **overrides: Any) -> dict:
        _, binding = await self.api("POST", "/api/bindings", self.http_binding(self.button["id"], **overrides))
        return binding

    async def test_a_press_runs_the_action_and_is_logged(self) -> None:
        await self.add_binding()
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2))
        await self.settle()
        self.assertEqual(self.hits, [{"method": "GET", "path": "/hit"}])
        _, log = await self.api("GET", "/api/log")
        kinds = [e["kind"] for e in log["entries"]]
        self.assertTrue({"ble", "fire", "action"} <= set(kinds), kinds)
        text = " ".join(e["text"] for e in log["entries"])
        self.assertIn("Desk remote", text)
        self.assertIn("HTTP 200", text)

    async def test_repeated_copies_of_a_press_run_the_action_once(self) -> None:
        await self.add_binding()
        for _ in range(25):
            self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2))
        await self.settle()
        self.assertEqual(len(self.hits), 1)

    async def test_other_buttons_and_unknown_devices_do_not_fire(self) -> None:
        await self.add_binding()
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B1))
        self.state.handle_packet("11:22:33:44:55:66", -40, bytes.fromhex(PRESS_B2))
        await self.settle()
        self.assertEqual(self.hits, [])
        _, log = await self.api("GET", "/api/log")
        self.assertTrue(any("unknown device 11:22:33:44:55:66" in e["text"] for e in log["entries"]))

    async def test_disabled_bindings_do_not_fire(self) -> None:
        await self.add_binding(enabled=False)
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2))
        await self.settle()
        self.assertEqual(self.hits, [])

    async def test_editing_a_binding_takes_effect_immediately(self) -> None:
        binding = await self.add_binding(enabled=False)
        await self.api("PUT", f"/api/bindings/{binding['id']}", self.http_binding(self.button["id"]))
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2))
        await self.settle()
        self.assertEqual(len(self.hits), 1)

    async def test_failing_action_is_logged_as_an_error(self) -> None:
        await self.add_binding(action={"type": "http", "method": "GET", "url": "http://127.0.0.1:1/x"})
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B2))
        await self.settle()
        _, log = await self.api("GET", "/api/log")
        self.assertTrue(any(e["kind"] == "error" and "Hit target" in e["text"] for e in log["entries"]))

    async def test_run_now(self) -> None:
        binding = await self.add_binding()
        status, data = await self.api("POST", f"/api/bindings/{binding['id']}/run")
        self.assertEqual(status, 200)
        self.assertTrue(data["ok"])
        self.assertIn("HTTP 200", data["summary"])
        self.assertEqual(len(self.hits), 1)
        self.assertEqual((await self.api("POST", "/api/bindings/ghost/run"))[0], 404)

    async def test_run_now_reports_failure(self) -> None:
        binding = await self.add_binding(action={"type": "http", "url": "http://127.0.0.1:1/x"})
        _, data = await self.api("POST", f"/api/bindings/{binding['id']}/run")
        self.assertFalse(data["ok"])

    async def test_simulate_fires_matching_bindings_ignoring_cooldown(self) -> None:
        await self.add_binding(name="First", cooldown_ms=60000)
        await self.add_binding(name="Second")
        trigger = {"button": self.button["id"], "input": "button:2", "event": "press", "channel": None}
        for _ in range(2):
            status, data = await self.api("POST", "/api/simulate", trigger)
            self.assertEqual((status, data["matched"]), (200, ["First", "Second"]))
        await self.settle()
        self.assertEqual(len(self.hits), 4)
        trigger["input"] = "button:3"
        self.assertEqual((await self.api("POST", "/api/simulate", trigger))[1]["matched"], [])
        trigger["button"] = "ghost"
        self.assertEqual((await self.api("POST", "/api/simulate", trigger))[0], 404)

    async def test_log_since(self) -> None:
        _, first = await self.api("GET", "/api/log")
        self.assertGreater(first["last_seq"], 0)
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B1))
        _, newer = await self.api("GET", f"/api/log?since={first['last_seq']}")
        self.assertEqual(len(newer["entries"]), 1)
        self.assertEqual((await self.api("GET", "/api/log?since=abc"))[0], 400)


class SettingsAndSwitchBotTest(ApiTestCase):
    async def asyncSetUp(self) -> None:
        await super().asyncSetUp()
        await self.sign_in()

    async def test_settings(self) -> None:
        status, data = await self.api("PUT", "/api/settings", {"http_timeout_s": 5})
        self.assertEqual((status, data["http_timeout_s"]), (200, 5.0))
        self.assertEqual((await self.api("PUT", "/api/settings", {"http_timeout_s": 0}))[0], 400)
        _, state = await self.api("GET", "/api/state")
        self.assertEqual(state["settings"]["http_timeout_s"], 5.0)

    async def test_credentials_are_write_only(self) -> None:
        status, data = await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})
        self.assertEqual((status, data), (200, {"configured": True}))
        _, state = await self.api("GET", "/api/state")
        self.assertEqual(state["switchbot"], {"configured": True})
        self.assertNotIn(TOKEN, json.dumps(state))
        self.assertNotIn(SECRET, json.dumps(state))
        _, log = await self.api("GET", "/api/log")
        self.assertNotIn(TOKEN, json.dumps(log))

    async def test_a_blank_field_keeps_the_stored_value(self) -> None:
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})
        await self.api("PUT", "/api/switchbot/credentials", {"token": "", "secret": ""})
        self.assertEqual((self.state.config.switchbot_token, self.state.config.switchbot_secret), (TOKEN, SECRET))
        await self.api("PUT", "/api/switchbot/credentials", {"token": "other"})
        self.assertEqual((self.state.config.switchbot_token, self.state.config.switchbot_secret), ("other", SECRET))

    async def test_credentials_can_be_removed(self) -> None:
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})
        self.assertEqual((await self.api("DELETE", "/api/switchbot/credentials"))[1], {"configured": False})
        self.assertFalse(self.state.config.has_switchbot_credentials)

    async def test_device_scene_and_command_lists(self) -> None:
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})
        _, devices = await self.api("GET", "/api/switchbot/devices")
        self.assertEqual([d["id"] for d in devices["devices"]], ["BOT1", "HUB1", "TV1"])
        _, scenes = await self.api("GET", "/api/switchbot/scenes")
        self.assertEqual(scenes["scenes"], [{"id": "S1", "name": "Good night"}])
        _, commands = await self.api("GET", "/api/switchbot/commands?device_id=BOT1")
        self.assertEqual([c["command"] for c in commands["commands"]], ["turnOff", "turnOn", "press"])
        self.assertEqual((await self.api("GET", "/api/switchbot/commands"))[0], 400)
        self.assertEqual((await self.api("GET", "/api/switchbot/commands?device_id=NOPE"))[0], 502)

    async def test_lists_fail_clearly_without_credentials_or_with_wrong_ones(self) -> None:
        status, data = await self.api("GET", "/api/switchbot/devices")
        self.assertEqual(status, 502)
        self.assertIn("not set", data["error"])
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": "wrong"})
        status, data = await self.api("GET", "/api/switchbot/devices")
        self.assertEqual(status, 502)
        self.assertIn("authentication failed", data["error"])

    async def test_changing_credentials_drops_the_cached_lists(self) -> None:
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})
        await self.api("GET", "/api/switchbot/devices")
        await self.api("PUT", "/api/switchbot/credentials", {"secret": "wrong"})
        self.assertEqual((await self.api("GET", "/api/switchbot/devices"))[0], 502)

    async def test_a_switchbot_binding_runs_through_the_api(self) -> None:
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})
        button = (await self.add_button())["button"]
        _, binding = await self.api("POST", "/api/bindings", {
            "name": "Bot", "trigger": {"button": button["id"], "input": "button:2", "event": "press"},
            "action": {"type": "switchbot_command", "device_id": "BOT1", "device_name": "Living Bot",
                       "command": "press"}})
        _, data = await self.api("POST", f"/api/bindings/{binding['id']}/run")
        self.assertTrue(data["ok"], data)
        self.assertEqual(self.fake_sb.requests[0][:2], ("command", "BOT1"))


class ExportImportTest(ApiTestCase):
    async def asyncSetUp(self) -> None:
        await super().asyncSetUp()
        await self.sign_in()
        self.button = (await self.add_button())["button"]
        await self.api("POST", "/api/bindings", self.http_binding(self.button["id"]))
        await self.api("PUT", "/api/switchbot/credentials", {"token": TOKEN, "secret": SECRET})

    async def export(self, credentials: bool = False) -> dict:
        response = await self.client.get("/api/export" + ("?credentials=1" if credentials else ""))
        self.assertEqual(response.status, 200)
        self.assertIn("attachment", response.headers["Content-Disposition"])
        return json.loads(await response.text())

    async def test_export_leaves_credentials_out_by_default(self) -> None:
        data = await self.export()
        self.assertEqual(len(data["buttons"]), 1)
        self.assertEqual(len(data["bindings"]), 1)
        self.assertNotIn("switchbot", data)
        self.assertNotIn(TOKEN, json.dumps(data))

    async def test_export_with_credentials_on_request(self) -> None:
        self.assertEqual((await self.export(credentials=True))["switchbot"], {"token": TOKEN, "secret": SECRET})

    async def test_export_never_contains_the_password_hash_or_key(self) -> None:
        text = json.dumps(await self.export(credentials=True))
        self.assertNotIn("scrypt", text)
        self.assertNotIn("password", text)

    async def test_export_then_import_replace_round_trips(self) -> None:
        data = await self.export()
        await self.api("DELETE", f"/api/buttons/{self.button['id']}?cascade=1")
        status, summary = await self.api("POST", "/api/import", {"config": data, "mode": "replace"})
        self.assertEqual((status, summary), (200, {"buttons": 1, "bindings": 1}))
        # Credentials weren't in the file, so the stored ones are untouched.
        self.assertTrue(self.state.config.has_switchbot_credentials)
        # The imported binding fires.
        self.clock.advance(60)
        self.state.handle_packet(DESK, -40, bytes.fromhex(PRESS_B1.replace("3a013a00", "3a003a01")))
        await self.settle()
        self.assertEqual(len(self.hits), 1)

    async def test_import_merge_adds_to_what_exists(self) -> None:
        extra = {"buttons": [{"id": "other", "name": "Other", "mac": "11:22:33:44:55:66"}], "bindings": []}
        status, summary = await self.api("POST", "/api/import", {"config": extra, "mode": "merge"})
        self.assertEqual((status, summary), (200, {"buttons": 2, "bindings": 1}))

    async def test_invalid_import_is_refused_and_changes_nothing(self) -> None:
        before = await self.export(credentials=True)
        for body in ({"config": {"bindings": [{"id": "x"}]}, "mode": "merge"},
                     {"config": {"version": 99}, "mode": "replace"},
                     {"config": {}, "mode": "overwrite"},
                     {"mode": "merge"}):
            self.assertEqual((await self.api("POST", "/api/import", body))[0], 400, body)
        self.assertEqual(await self.export(credentials=True), before)


class StaticPageTest(ApiTestCase):
    async def test_index_is_served_without_signing_in(self) -> None:
        page = Path(__file__).parent / "static" / "index.html"
        if not page.exists():
            self.skipTest("static/index.html has not been written yet")
        response = await self.client.get("/")
        self.assertEqual(response.status, 200)
        self.assertIn("text/html", response.headers["Content-Type"])
        self.assertEqual(response.headers["Cache-Control"], "no-cache")
        for name in ("app.js", "style.css"):
            self.assertEqual((await self.client.get(f"/static/{name}")).status, 200)

    async def test_index_page_has_no_inline_scripts_or_handlers(self) -> None:
        page = Path(__file__).parent / "static" / "index.html"
        if not page.exists():
            self.skipTest("static/index.html has not been written yet")
        html = page.read_text(encoding="utf-8")
        self.assertNotIn("onclick=", html.lower())
        self.assertNotIn(' style="', html.lower())
        self.assertNotRegex(html, r"<script(?![^>]*\bsrc=)")


class RestartTest(ApiTestCase):
    async def test_password_and_config_survive_a_restart(self) -> None:
        await self.sign_in()
        button = (await self.add_button())["button"]
        await self.api("POST", "/api/bindings", self.http_binding(button["id"]))
        await self.client.close()
        await self.open_app()
        self.assertEqual((await self.api("GET", "/api/state"))[0], 401)
        self.assertEqual((await self.api("POST", "/api/auth/login", {"password": PASSWORD}))[0], 200)
        _, state = await self.api("GET", "/api/state")
        self.assertEqual(len(state["buttons"]), 1)
        self.assertEqual(len(state["bindings"]), 1)


if __name__ == "__main__":
    unittest.main()
