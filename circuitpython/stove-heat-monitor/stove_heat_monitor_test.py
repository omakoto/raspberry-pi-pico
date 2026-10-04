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
for mod in ["board", "busio", "digitalio", "wifi", "socketpool", "adafruit_requests", "adafruit_mlx90640", "common"]:
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


if __name__ == "__main__":
    unittest.main()
