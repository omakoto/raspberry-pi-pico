"""Unit tests for actions.py, against fake local HTTP and SwitchBot servers.

Run from the project directory: python3 -m unittest discover -p '*_test.py'
Needs aiohttp (the tests are skipped without it).
"""

import asyncio
import unittest

try:
    import aiohttp
    from aiohttp import web
    from aiohttp.test_utils import TestServer
except ImportError:
    raise unittest.SkipTest("aiohttp is not installed")

from button_dispatcher import actions, switchbot
from button_dispatcher.config import Config
from button_dispatcher.switchbot_test import SECRET, TOKEN, FakeSwitchBot


class ActionRunnerTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.seen: list[dict] = []
        app = web.Application()
        app.router.add_route("*", "/hit", self.hit)
        app.router.add_get("/redirect", self.redirect)
        app.router.add_get("/slow", self.slow)
        app.router.add_get("/missing", self.missing)
        self.server = TestServer(app)
        await self.server.start_server()
        self.sb_api = FakeSwitchBot()
        await self.sb_api.server.start_server()
        self.session = aiohttp.ClientSession()
        self.config = Config()
        self.config.switchbot_token, self.config.switchbot_secret = TOKEN, SECRET
        service = switchbot.SwitchBotService(
            lambda: (self.config.switchbot_token, self.config.switchbot_secret),
            lambda: self.session, api_base=self.sb_api.api_base)
        self.runner = actions.ActionRunner(lambda: self.config, lambda: self.session, service)

    async def asyncTearDown(self) -> None:
        await self.session.close()
        await self.server.close()
        await self.sb_api.server.close()

    async def hit(self, request: web.Request) -> web.Response:
        self.seen.append({"method": request.method, "path": request.path_qs,
                          "headers": dict(request.headers), "body": await request.read()})
        return web.Response(text="ok")

    async def redirect(self, request: web.Request) -> web.Response:
        raise web.HTTPFound("/hit?from=redirect")

    async def missing(self, request: web.Request) -> web.Response:
        return web.Response(status=404)

    async def slow(self, request: web.Request) -> web.Response:
        await asyncio.sleep(5)
        return web.Response(text="late")

    def url(self, path: str) -> str:
        return str(self.server.make_url(path))

    async def test_get(self) -> None:
        result = await self.runner.run({"type": "http", "method": "GET", "url": self.url("/hit?x=1")})
        self.assertTrue(result.ok)
        self.assertIn("HTTP 200", result.summary)
        self.assertEqual((self.seen[0]["method"], self.seen[0]["path"]), ("GET", "/hit?x=1"))

    async def test_post_with_body_and_headers(self) -> None:
        result = await self.runner.run({"type": "http", "method": "POST", "url": self.url("/hit"),
                                        "headers": {"X-Token": "abc"}, "body": '{"on": true}'})
        self.assertTrue(result.ok)
        seen = self.seen[0]
        self.assertEqual((seen["method"], seen["body"]), ("POST", b'{"on": true}'))
        self.assertEqual(seen["headers"]["X-Token"], "abc")
        self.assertEqual(seen["headers"]["Content-Type"], "application/json")

    async def test_explicit_content_type_is_kept(self) -> None:
        await self.runner.run({"type": "http", "method": "POST", "url": self.url("/hit"),
                               "headers": {"content-type": "text/x-custom"}, "body": "{}"})
        self.assertEqual(self.seen[0]["headers"]["Content-Type"], "text/x-custom")

    async def test_plain_text_body_gets_text_content_type(self) -> None:
        await self.runner.run({"type": "http", "method": "PUT", "url": self.url("/hit"), "body": "hello"})
        self.assertTrue(self.seen[0]["headers"]["Content-Type"].startswith("text/plain"))

    async def test_redirects_are_followed(self) -> None:
        result = await self.runner.run({"type": "http", "url": self.url("/redirect")})
        self.assertTrue(result.ok)
        self.assertEqual(self.seen[0]["path"], "/hit?from=redirect")

    async def test_error_status_is_a_failure(self) -> None:
        result = await self.runner.run({"type": "http", "url": self.url("/missing")})
        self.assertFalse(result.ok)
        self.assertIn("HTTP 404", result.summary)

    async def test_timeout(self) -> None:
        result = await self.runner.run({"type": "http", "url": self.url("/slow"), "timeout_s": 0.2})
        self.assertFalse(result.ok)
        self.assertIn("timed out", result.summary)

    async def test_config_timeout_is_the_default(self) -> None:
        self.config.settings["http_timeout_s"] = 0.2
        result = await self.runner.run({"type": "http", "url": self.url("/slow")})
        self.assertIn("timed out", result.summary)

    async def test_unreachable_host(self) -> None:
        result = await self.runner.run({"type": "http", "url": "http://127.0.0.1:1/x"})
        self.assertFalse(result.ok)

    async def test_secret_url_parts_never_appear_in_results(self) -> None:
        secret_url = "http://127.0.0.1:1/trigger/event/with/key/SUPERSECRETKEY?token=SUPERSECRETKEY"
        result = await self.runner.run({"type": "http", "url": secret_url})
        self.assertNotIn("SUPERSECRETKEY", result.summary)
        ok = await self.runner.run({"type": "http", "url": self.url("/hit?key=SUPERSECRETKEY")})
        self.assertNotIn("SUPERSECRETKEY", ok.summary)

    async def test_switchbot_command(self) -> None:
        result = await self.runner.run({"type": "switchbot_command", "device_id": "BOT1",
                                        "device_name": "Living Bot", "command": "press",
                                        "parameter": "default", "command_type": "command"})
        self.assertTrue(result.ok, result.summary)
        self.assertIn("Living Bot", result.summary)
        self.assertEqual(self.sb_api.requests[0][:2], ("command", "BOT1"))

    async def test_switchbot_scene(self) -> None:
        result = await self.runner.run({"type": "switchbot_scene", "scene_id": "S1",
                                        "scene_name": "Good night"})
        self.assertTrue(result.ok, result.summary)
        self.assertIn("Good night", result.summary)

    async def test_switchbot_errors_become_failed_results(self) -> None:
        self.sb_api.status_code, self.sb_api.message = 161, "device offline"
        result = await self.runner.run({"type": "switchbot_command", "device_id": "BOT1",
                                        "command": "press"})
        self.assertFalse(result.ok)
        self.assertIn("161", result.summary)

    async def test_switchbot_without_credentials(self) -> None:
        self.config.switchbot_token = ""
        result = await self.runner.run({"type": "switchbot_scene", "scene_id": "S1"})
        self.assertFalse(result.ok)
        self.assertIn("not set", result.summary)

    async def test_unknown_action_type(self) -> None:
        result = await self.runner.run({"type": "teleport"})
        self.assertFalse(result.ok)


class HostOfTest(unittest.TestCase):
    def test_host_only(self) -> None:
        self.assertEqual(actions.host_of("https://maker.ifttt.com/trigger/x/with/key/K?q=1"),
                         "https://maker.ifttt.com")
        self.assertEqual(actions.host_of("http://wol.local:8080/wake"), "http://wol.local:8080")
        self.assertEqual(actions.host_of("garbage"), "<url>")


if __name__ == "__main__":
    unittest.main()
