"""Unit tests for switchbot.py and switchbot_commands.py, against a fake local SwitchBot API.

Run from the project directory: python3 -m unittest discover -p '*_test.py'
Needs aiohttp (the tests are skipped without it).
"""

import base64
import hashlib
import hmac
import unittest

try:
    import aiohttp
    from aiohttp import web
    from aiohttp.test_utils import TestServer
except ImportError:
    raise unittest.SkipTest("aiohttp is not installed")

from button_dispatcher import switchbot, switchbot_commands

TOKEN = "test-token"
SECRET = "test-secret"

DEVICES_BODY = {
    "deviceList": [
        {"deviceId": "BOT1", "deviceName": "Living Bot", "deviceType": "Bot", "hubDeviceId": "HUB1"},
        {"deviceId": "HUB1", "deviceName": "Hub Mini", "deviceType": "Hub Mini", "hubDeviceId": "HUB1"},
    ],
    "infraredRemoteList": [
        {"deviceId": "TV1", "deviceName": "TV", "remoteType": "TV", "hubDeviceId": "HUB1"},
    ],
}
SCENES_BODY = [{"sceneId": "S1", "sceneName": "Good night"}]


class FakeSwitchBot:
    """A stand-in for the SwitchBot API that verifies the signature like the real one."""

    def __init__(self) -> None:
        self.requests: list[tuple[str, str, bytes]] = []
        self.status_code = 100
        self.message = "success"
        self.http_status = 200
        self.devices_calls = 0
        app = web.Application()
        app.router.add_get("/v1.1/devices", self.devices)
        app.router.add_get("/v1.1/scenes", self.scenes)
        app.router.add_post("/v1.1/devices/{id}/commands", self.command)
        app.router.add_post("/v1.1/scenes/{id}/execute", self.execute)
        self.server = TestServer(app)

    def signature_ok(self, request: web.Request) -> bool:
        token, t, nonce = request.headers.get("Authorization", ""), request.headers.get("t", ""), request.headers.get("nonce", "")
        expected = base64.b64encode(
            hmac.new(SECRET.encode(), (TOKEN + t + nonce).encode(), hashlib.sha256).digest()).decode()
        return token == TOKEN and bool(t) and bool(nonce) and request.headers.get("sign") == expected

    def respond(self, request: web.Request, body) -> web.Response:
        if not self.signature_ok(request):
            return web.Response(status=401)
        if self.http_status != 200:
            return web.Response(status=self.http_status)
        return web.json_response({"statusCode": self.status_code, "message": self.message, "body": body})

    async def devices(self, request: web.Request) -> web.Response:
        self.devices_calls += 1
        return self.respond(request, DEVICES_BODY)

    async def scenes(self, request: web.Request) -> web.Response:
        return self.respond(request, SCENES_BODY)

    async def command(self, request: web.Request) -> web.Response:
        self.requests.append(("command", request.match_info["id"], await request.read()))
        return self.respond(request, {})

    async def execute(self, request: web.Request) -> web.Response:
        self.requests.append(("scene", request.match_info["id"], await request.read()))
        return self.respond(request, {})

    @property
    def api_base(self) -> str:
        return str(self.server.make_url("/v1.1")).rstrip("/")


class MakeAuthHeadersTest(unittest.TestCase):
    def test_signature_is_base64_hmac_of_token_time_nonce(self) -> None:
        h = switchbot.make_auth_headers("tok", "sec", t="1700000000000", nonce="abc")
        expected = base64.b64encode(
            hmac.new(b"sec", b"tok1700000000000abc", hashlib.sha256).digest()).decode()
        self.assertEqual(h["sign"], expected)
        self.assertEqual((h["Authorization"], h["t"], h["nonce"]), ("tok", "1700000000000", "abc"))

    def test_nonce_and_timestamp_are_generated(self) -> None:
        a = switchbot.make_auth_headers("tok", "sec")
        b = switchbot.make_auth_headers("tok", "sec")
        self.assertNotEqual(a["nonce"], b["nonce"])
        self.assertEqual(len(a["t"]), 13)


class ClientTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.api = FakeSwitchBot()
        await self.api.server.start_server()
        self.session = aiohttp.ClientSession()
        self.client = switchbot.SwitchBotClient(TOKEN, SECRET, self.session, api_base=self.api.api_base)

    async def asyncTearDown(self) -> None:
        await self.session.close()
        await self.api.server.close()

    async def test_lists_devices_and_scenes(self) -> None:
        self.assertEqual((await self.client.devices())["deviceList"][0]["deviceId"], "BOT1")
        self.assertEqual((await self.client.scenes())[0]["sceneId"], "S1")

    async def test_command_body(self) -> None:
        await self.client.command("BOT1", "press")
        kind, device, body = self.api.requests[0]
        self.assertEqual((kind, device), ("command", "BOT1"))
        self.assertEqual(body, b'{"command": "press", "parameter": "default", "commandType": "command"}')

    async def test_execute_scene(self) -> None:
        await self.client.execute_scene("S1")
        self.assertEqual(self.api.requests[0][:2], ("scene", "S1"))

    async def test_wrong_credentials_are_reported(self) -> None:
        bad = switchbot.SwitchBotClient(TOKEN, "wrong", self.session, api_base=self.api.api_base)
        with self.assertRaisesRegex(switchbot.SwitchBotError, "authentication failed"):
            await bad.devices()

    async def test_api_error_codes_are_explained(self) -> None:
        self.api.status_code, self.api.message = 161, "device offline"
        with self.assertRaisesRegex(switchbot.SwitchBotError, "161"):
            await self.client.command("BOT1", "press")
        self.api.status_code, self.api.message = 190, "bad parameter"
        with self.assertRaisesRegex(switchbot.SwitchBotError, "invalid parameter"):
            await self.client.command("BOT1", "press")

    async def test_http_error_status(self) -> None:
        self.api.http_status = 500
        with self.assertRaisesRegex(switchbot.SwitchBotError, "HTTP 500"):
            await self.client.devices()

    async def test_unreachable_server(self) -> None:
        dead = switchbot.SwitchBotClient(TOKEN, SECRET, self.session, api_base="http://127.0.0.1:1/v1.1")
        with self.assertRaisesRegex(switchbot.SwitchBotError, "cannot reach"):
            await dead.devices()


class ServiceTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.api = FakeSwitchBot()
        await self.api.server.start_server()
        self.session = aiohttp.ClientSession()
        self.now = 0.0
        self.credentials = (TOKEN, SECRET)
        self.service = switchbot.SwitchBotService(
            lambda: self.credentials, lambda: self.session, api_base=self.api.api_base,
            clock=lambda: self.now)

    async def asyncTearDown(self) -> None:
        await self.session.close()
        await self.api.server.close()

    async def test_devices_are_simplified_and_include_ir_remotes(self) -> None:
        devices = await self.service.devices()
        self.assertEqual([(d["id"], d["type"], d["remote_type"]) for d in devices],
                         [("BOT1", "Bot", None), ("HUB1", "Hub Mini", None), ("TV1", "IR TV", "TV")])

    async def test_scenes(self) -> None:
        self.assertEqual(await self.service.scenes(), [{"id": "S1", "name": "Good night"}])

    async def test_lists_are_cached_until_refreshed_or_expired(self) -> None:
        await self.service.devices()
        await self.service.devices()
        self.assertEqual(self.api.devices_calls, 1)
        await self.service.devices(refresh=True)
        self.assertEqual(self.api.devices_calls, 2)
        self.now += switchbot.SwitchBotService.CACHE_TTL_S + 1
        await self.service.devices()
        self.assertEqual(self.api.devices_calls, 3)

    async def test_invalidate_drops_the_cache(self) -> None:
        await self.service.devices()
        self.service.invalidate()
        await self.service.devices()
        self.assertEqual(self.api.devices_calls, 2)

    async def test_missing_credentials(self) -> None:
        self.credentials = ("", "")
        with self.assertRaisesRegex(switchbot.SwitchBotError, "not set"):
            await self.service.devices()

    async def test_commands_for_a_device(self) -> None:
        info = await self.service.commands_for("BOT1")
        self.assertEqual([c["command"] for c in info["commands"]], ["turnOff", "turnOn", "press"])
        self.assertEqual(info["device"]["name"], "Living Bot")
        ir = await self.service.commands_for("TV1")
        self.assertIn("SetChannel", [c["command"] for c in ir["commands"]])
        self.assertTrue(ir["notes"])

    async def test_commands_for_an_unknown_device(self) -> None:
        with self.assertRaisesRegex(switchbot.SwitchBotError, "no device"):
            await self.service.commands_for("NOPE")


class CommandTableTest(unittest.TestCase):
    def test_no_passcode_field_descriptions_pose_as_device_types(self) -> None:
        for bogus in ("endTime", "id", "Key Name", "name", "password", "startTime", "type"):
            self.assertNotIn(bogus, switchbot_commands.DEVICE_COMMANDS)

    def test_rows_are_unique_and_well_formed(self) -> None:
        tables = list(switchbot_commands.DEVICE_COMMANDS.values()) + list(
            switchbot_commands.IR_COMMANDS.values()) + [switchbot_commands.IR_COMMON_COMMANDS]
        for rows in tables:
            self.assertEqual(len(rows), len(set(rows)))
            for command, parameter, description in rows:
                self.assertTrue(command.strip())
                self.assertIsInstance(parameter, str)
                self.assertIsInstance(description, str)
                for text in (command, parameter, description):
                    self.assertFalse(set("“”‘’") & set(text), text)

    def test_unknown_device_type_explains_itself(self) -> None:
        rows, notes = switchbot_commands.commands_for("Gizmo 9000")
        self.assertEqual(rows, [])
        self.assertIn("type a command by hand", notes[0])

    def test_ir_remote_types(self) -> None:
        rows, notes = switchbot_commands.commands_for("IR TV", "TV")
        names = [r[0] for r in rows]
        self.assertEqual(names[:2], ["turnOn", "turnOff"])
        self.assertIn("SetChannel", names)
        self.assertEqual(switchbot_commands.commands_for("IR Others", "Others")[0], [])
        diy, diy_notes = switchbot_commands.commands_for("IR DIY Fan", "DIY Fan")
        self.assertIn("swing", [r[0] for r in diy])
        self.assertEqual(len(diy_notes), 2)


if __name__ == "__main__":
    unittest.main()
