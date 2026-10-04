#!/usr/bin/env python3
#
# Unit test for Kitchen Stove Heat Monitor configuration parser and formatting logic.

import importlib.util
import os
import sys
import tempfile
import types
import unittest
from unittest.mock import MagicMock

# Mock CircuitPython hardware modules before importing stove-heat-monitor
for mod in ["board", "busio", "digitalio", "wifi", "socketpool", "adafruit_requests", "adafruit_mlx90640", "common", "supervisor", "mdns"]:
    if mod not in sys.modules:
        sys.modules[mod] = MagicMock()

# Dynamically load stove-heat-monitor.py
script_path = os.path.join(os.path.dirname(__file__), "stove-heat-monitor.py")
spec = importlib.util.spec_from_file_location("stove_heat_monitor", script_path)
stove_heat_monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stove_heat_monitor)


class TestStoveHeatMonitor(unittest.TestCase):

    def test_parse_toml_file(self) -> None:
        sample_toml = """
        # Base settings
        wifi_ssid = "TestSSID"
        wifi_password = "SecretPassword"
        threshold_c = 60.5 # Float threshold
        alert_delay_minutes = 15.0
        monitoring_interval_s = 2.0
        dump_matrix = true
        color_matrix = false
        i2c_frequency = 400000
        i2c_scl = 6
        """
        with tempfile.NamedTemporaryFile("w", delete=False, suffix=".toml") as f:
            f.write(sample_toml)
            temp_path = f.name

        try:
            config: dict[str, str | int | float | bool] = {}
            success = stove_heat_monitor.parse_toml_file(temp_path, config)
            self.assertTrue(success)
            self.assertEqual(config.get("wifi_ssid"), "TestSSID")
            self.assertEqual(config.get("wifi_password"), "SecretPassword")
            self.assertEqual(config.get("threshold_c"), 60.5)
            self.assertEqual(config.get("alert_delay_minutes"), 15.0)
            self.assertEqual(config.get("monitoring_interval_s"), 2.0)
            self.assertEqual(config.get("dump_matrix"), True)
            self.assertEqual(config.get("color_matrix"), False)
            self.assertEqual(config.get("i2c_frequency"), 400000)
            self.assertEqual(config.get("i2c_scl"), 6)
        finally:
            os.remove(temp_path)

    def test_render_matrix(self) -> None:
        # Create a mock 768-element thermal frame
        frame = [25.0] * 768
        # Put hot spots in the middle
        frame[12 * 32 + 16] = 75.2
        frame[12 * 32 + 17] = 85.0
        # Should execute cleanly without error
        stove_heat_monitor.render_matrix_to_serial(frame, threshold_c=60.0, colorize=False)
        stove_heat_monitor.render_matrix_to_serial(frame, threshold_c=60.0, colorize=True)

    def test_format_matrix_and_request_path(self) -> None:
        frame = [25.0] * 768
        frame[0] = 85.4
        text = stove_heat_monitor.format_matrix(frame, 60.0, False)
        self.assertNotIn("\033", text)
        self.assertEqual(len(text.split("\n")), 3 + 24 + 1)
        self.assertIn("00 | 85 25", text)
        self.assertIn("\033[97;41m", stove_heat_monitor.format_matrix(frame, 60.0, True))

        server = stove_heat_monitor.FrameServer.__new__(stove_heat_monitor.FrameServer)
        server.threshold_c, server.status, server.frame = 60.0, "st", frame
        server.refresh_s = 2
        self.assertNotIn("\033", server.build_text())
        html = server.build_html()
        self.assertTrue(html.startswith("<!DOCTYPE html>"))
        self.assertIn('<span class="h">85</span>', html)
        self.assertIn('<span class="b">25</span>', html)
        self.assertIn("<pre>st", html)

        parse = stove_heat_monitor.parse_request_path
        self.assertEqual(parse(b"GET /frame?x=1 HTTP/1.1\r\nHost: a\r\n\r\n"), "/frame")
        self.assertEqual(parse(b"GET / HTTP/1.1\r\n\r\n"), "/")
        self.assertIsNone(parse(b"POST / HTTP/1.1\r\n\r\n"))
        self.assertIsNone(parse(b""))

    def test_log_config(self) -> None:
        import io
        from contextlib import redirect_stdout

        test_config: dict[str, str | int | float | bool] = {
            "wifi_ssid": "MySSID",
            "wifi_password": "supersecretpassword",
            "pushover_token": "a1b2c3d4e5",
            "pushover_user_key": "u6v7w8x9y0",
            "pushover_url": "https://api.pushover.net/1/messages.json",
            "threshold_c": 55.0,
            "dump_matrix": True,
        }

        buf = io.StringIO()
        with redirect_stdout(buf):
            stove_heat_monitor.log_config(test_config)

        output = buf.getvalue()
        # Verify sensitive values are masked
        self.assertNotIn("supersecretpassword", output)
        self.assertNotIn("a1b2c3d4e5", output)
        self.assertNotIn("u6v7w8x9y0", output)
        self.assertIn("wifi_password: ***", output)
        self.assertIn("pushover_token: ***", output)
        self.assertIn("pushover_user_key: ***", output)

        # Verify non-sensitive values are logged cleanly
        self.assertIn("wifi_ssid: MySSID", output)
        self.assertIn("pushover_url: https://api.pushover.net/1/messages.json", output)
        self.assertIn("threshold_c: 55.0", output)
        self.assertIn("dump_matrix: True", output)

    def test_urlencode_val(self) -> None:
        import urllib.parse

        test_cases = [
            "Hello World",
            "Stove surface has been HOT (max 61.8°C, threshold 35°C) for 3 minutes!",
            "abc123-_.~",
            "Special & = + ? % chars",
            123,
            45.6,
        ]
        for val in test_cases:
            expected = urllib.parse.quote_plus(str(val))
            actual = stove_heat_monitor.urlencode_val(val)
            self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
