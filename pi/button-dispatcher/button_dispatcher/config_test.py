"""Unit tests for config.py (validation, import/export, storage).

Run from the project directory: python3 -m unittest discover -p '*_test.py'
"""

import copy
import json
import os
import stat
import tempfile
import unittest
from pathlib import Path

from button_dispatcher import config as cfg
from button_dispatcher.config import ConfigError

BUTTON = {"id": "desk", "name": "Desk remote", "mac": "aa:bb:cc:dd:ee:01", "model": "SBBT-104CUS"}
BINDING = {
    "id": "wake", "name": "Wake PC", "enabled": True, "cooldown_ms": 700,
    "trigger": {"button": "desk", "channel": None, "input": "button:2", "event": "press"},
    "action": {"type": "http", "method": "GET", "url": "http://wol.local/wake"},
}


def doc() -> dict:
    return {"version": 1, "buttons": [copy.deepcopy(BUTTON)], "bindings": [copy.deepcopy(BINDING)]}


class ParseActionTest(unittest.TestCase):
    def test_http_get_defaults(self) -> None:
        a = cfg.parse_action({"type": "http", "url": "https://example.com/x?y=1"})
        self.assertEqual(a, {"type": "http", "method": "GET", "url": "https://example.com/x?y=1"})

    def test_http_full(self) -> None:
        a = cfg.parse_action({"type": "http", "method": "post", "url": "http://h/p",
                              "headers": {"Content-Type": "application/json"}, "body": "{}",
                              "timeout_s": 5, "ignored": "x"})
        self.assertEqual(a["method"], "POST")
        self.assertEqual(a["headers"], {"Content-Type": "application/json"})
        self.assertEqual(a["body"], "{}")
        self.assertEqual(a["timeout_s"], 5.0)
        self.assertNotIn("ignored", a)

    def test_http_rejects_bad_input(self) -> None:
        bad = [
            {"type": "http", "url": "ftp://example.com"},
            {"type": "http", "url": "not a url"},
            {"type": "http", "url": "http://"},
            {"type": "http", "url": ""},
            {"type": "http", "url": "http://h/", "method": "TRACE"},
            {"type": "http", "url": "http://h/", "headers": {"Bad Name": "x"}},
            {"type": "http", "url": "http://h/", "headers": {"X": "a\r\nb"}},
            {"type": "http", "url": "http://h/", "timeout_s": 0},
            {"type": "http", "url": "http://h/", "timeout_s": 61},
            {"type": "http", "url": "http://h/", "body": "x" * (cfg.MAX_BODY_LEN + 1)},
            {"type": "nope"},
            "string",
        ]
        for action in bad:
            with self.assertRaises(ConfigError, msg=str(action)):
                cfg.parse_action(action)

    def test_switchbot_command(self) -> None:
        a = cfg.parse_action({"type": "switchbot_command", "device_id": "ABC123", "command": "press",
                              "device_name": "Living Bot"})
        self.assertEqual(a, {"type": "switchbot_command", "device_id": "ABC123", "command": "press",
                             "device_name": "Living Bot", "parameter": "default",
                             "command_type": "command"})

    def test_switchbot_command_json_parameter_and_customize(self) -> None:
        a = cfg.parse_action({"type": "switchbot_command", "device_id": "X", "command": "setColor",
                              "parameter": {"r": 1}, "command_type": "customize"})
        self.assertEqual(a["parameter"], {"r": 1})
        self.assertEqual(a["command_type"], "customize")

    def test_switchbot_command_rejects_bad_input(self) -> None:
        for action in ({"type": "switchbot_command", "command": "press"},
                       {"type": "switchbot_command", "device_id": "X"},
                       {"type": "switchbot_command", "device_id": "X", "command": "p",
                        "command_type": "other"}):
            with self.assertRaises(ConfigError):
                cfg.parse_action(action)

    def test_switchbot_scene(self) -> None:
        a = cfg.parse_action({"type": "switchbot_scene", "scene_id": "S1", "scene_name": "Night"})
        self.assertEqual(a, {"type": "switchbot_scene", "scene_id": "S1", "scene_name": "Night"})
        with self.assertRaises(ConfigError):
            cfg.parse_action({"type": "switchbot_scene"})


class ParseTriggerTest(unittest.TestCase):
    def test_valid_triggers(self) -> None:
        for trigger_input, event in (("button:1", "press"), ("button:4", "hold_press"),
                                     ("wheel:left", "turn"), ("wheel:right", "turn")):
            t = cfg.parse_trigger({"button": "desk", "input": trigger_input, "event": event})
            self.assertIsNone(t.channel)

    def test_channel(self) -> None:
        t = cfg.parse_trigger({"button": "d", "input": "button:1", "event": "press", "channel": 3})
        self.assertEqual(t.channel, 3)

    def test_invalid_triggers(self) -> None:
        bad = [
            {"button": "d", "input": "button:0", "event": "press"},
            {"button": "d", "input": "wheel:up", "event": "turn"},
            {"button": "d", "input": "button:1", "event": "turn"},
            {"button": "d", "input": "wheel:left", "event": "press"},
            {"button": "d", "input": "button:1", "event": "kick"},
            {"button": "d", "input": "button:1", "event": "press", "channel": -1},
            {"button": "d", "input": "button:1", "event": "press", "channel": "1"},
            {"button": "d", "input": "button:1", "event": "press", "channel": True},
            {"button": "bad id!", "input": "button:1", "event": "press"},
        ]
        for trigger in bad:
            with self.assertRaises(ConfigError, msg=str(trigger)):
                cfg.parse_trigger(trigger)


class ConfigFromDictTest(unittest.TestCase):
    def test_valid_document_round_trips(self) -> None:
        config = cfg.config_from_dict(doc())
        self.assertEqual(config.buttons[0].mac, "AA:BB:CC:DD:EE:01")
        self.assertEqual(config.bindings[0].trigger.input, "button:2")
        out = cfg.config_to_dict(config, include_credentials=False)
        self.assertEqual(cfg.config_from_dict(out).bindings[0].id, "wake")

    def test_empty_document_is_valid(self) -> None:
        config = cfg.config_from_dict({})
        self.assertEqual((config.buttons, config.bindings), ([], []))
        self.assertEqual(config.settings["http_timeout_s"], 10.0)

    def test_binding_must_refer_to_a_known_button(self) -> None:
        d = doc()
        d["bindings"][0]["trigger"]["button"] = "ghost"
        with self.assertRaises(ConfigError):
            cfg.config_from_dict(d)

    def test_duplicate_ids_and_macs_are_rejected(self) -> None:
        d = doc()
        d["buttons"].append(dict(BUTTON, mac="aa:bb:cc:dd:ee:02"))
        with self.assertRaises(ConfigError):
            cfg.config_from_dict(d)
        d = doc()
        d["buttons"].append(dict(BUTTON, id="desk2"))
        with self.assertRaises(ConfigError):
            cfg.config_from_dict(d)
        d = doc()
        d["bindings"].append(copy.deepcopy(BINDING))
        with self.assertRaises(ConfigError):
            cfg.config_from_dict(d)

    def test_unsupported_version(self) -> None:
        with self.assertRaises(ConfigError):
            cfg.config_from_dict({"version": 2})

    def test_cooldown_validation(self) -> None:
        for bad in (-1, 60001, "5", 1.5, True):
            d = doc()
            d["bindings"][0]["cooldown_ms"] = bad
            with self.assertRaises(ConfigError, msg=str(bad)):
                cfg.config_from_dict(d)

    def test_settings_validation(self) -> None:
        self.assertEqual(cfg.config_from_dict({"settings": {"http_timeout_s": 5}}).settings["http_timeout_s"], 5.0)
        for bad in (0, 100, "x", True):
            with self.assertRaises(ConfigError, msg=str(bad)):
                cfg.config_from_dict({"settings": {"http_timeout_s": bad}})

    def test_credentials_are_exported_only_when_asked(self) -> None:
        d = doc()
        d["switchbot"] = {"token": "tok", "secret": "sec"}
        config = cfg.config_from_dict(d)
        self.assertEqual((config.switchbot_token, config.switchbot_secret), ("tok", "sec"))
        self.assertNotIn("switchbot", cfg.config_to_dict(config, include_credentials=False))
        self.assertEqual(cfg.config_to_dict(config, include_credentials=True)["switchbot"],
                         {"token": "tok", "secret": "sec"})
        self.assertNotIn("tok", json.dumps(cfg.config_to_dict(config, include_credentials=False)))


class ImportTest(unittest.TestCase):
    def current(self) -> cfg.Config:
        config = cfg.config_from_dict(doc())
        config.switchbot_token, config.switchbot_secret = "old-token", "old-secret"
        return config

    def test_replace_swaps_everything_but_keeps_credentials(self) -> None:
        other = {"buttons": [dict(BUTTON, id="b2", mac="11:22:33:44:55:66")], "bindings": []}
        result = cfg.import_config(self.current(), other, "replace")
        self.assertEqual([b.id for b in result.buttons], ["b2"])
        self.assertEqual(result.bindings, [])
        self.assertEqual(result.switchbot_token, "old-token")

    def test_replace_uses_imported_credentials_when_present(self) -> None:
        other = {"switchbot": {"token": "new", "secret": "newer"}}
        result = cfg.import_config(self.current(), other, "replace")
        self.assertEqual((result.switchbot_token, result.switchbot_secret), ("new", "newer"))

    def test_merge_adds_and_overwrites_by_id(self) -> None:
        extra_binding = dict(copy.deepcopy(BINDING), id="two", name="Second")
        changed = dict(copy.deepcopy(BINDING), name="Renamed")
        other = {"buttons": [], "bindings": [changed, extra_binding]}
        result = cfg.import_config(self.current(), other, "merge")
        self.assertEqual([b.id for b in result.buttons], ["desk"])
        self.assertEqual({b.id: b.name for b in result.bindings}, {"wake": "Renamed", "two": "Second"})

    def test_merge_allows_binding_to_an_existing_button(self) -> None:
        other = {"bindings": [dict(copy.deepcopy(BINDING), id="three")]}
        result = cfg.import_config(self.current(), other, "merge")
        self.assertEqual(len(result.bindings), 2)

    def test_merge_rejects_a_mac_clash_with_another_button(self) -> None:
        other = {"buttons": [dict(BUTTON, id="clone")]}
        with self.assertRaises(ConfigError):
            cfg.import_config(self.current(), other, "merge")

    def test_invalid_import_changes_nothing(self) -> None:
        current = self.current()
        before = cfg.config_to_dict(current, True)
        with self.assertRaises(ConfigError):
            cfg.import_config(current, {"bindings": [{"id": "x"}]}, "merge")
        with self.assertRaises(ConfigError):
            cfg.import_config(current, doc(), "overwrite")
        self.assertEqual(cfg.config_to_dict(current, True), before)


class StoreTest(unittest.TestCase):
    def test_missing_file_gives_an_empty_config(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            store = cfg.ConfigStore(Path(d) / "config.json")
            store.load()
            self.assertEqual(store.config.buttons, [])

    def test_save_and_load_with_private_permissions(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "sub" / "config.json"
            store = cfg.ConfigStore(path)
            store.config = cfg.config_from_dict(doc())
            store.config.switchbot_token = "tok"
            store.config.switchbot_secret = "sec"
            store.save()
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
            self.assertFalse((path.parent / "config.json.tmp").exists())
            again = cfg.ConfigStore(path)
            again.load()
            self.assertEqual(again.config.bindings[0].id, "wake")
            self.assertEqual(again.config.switchbot_token, "tok")

    def test_corrupt_file_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "config.json"
            path.write_text("{not json")
            with self.assertRaises(ConfigError):
                cfg.ConfigStore(path).load()


if __name__ == "__main__":
    unittest.main()
