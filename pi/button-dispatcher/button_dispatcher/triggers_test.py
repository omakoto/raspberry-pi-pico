"""Unit tests for triggers.py (dedupe, wheel handling, matching, cooldown) and models.py.

Run from the project directory: python3 -m unittest discover -p '*_test.py'
"""

import unittest
from typing import Optional

from button_dispatcher.models import Binding, Button, Trigger, normalize_mac
from button_dispatcher.triggers import TriggerEngine

DESK = "AA:BB:CC:DD:EE:01"
OTHER = "AA:BB:CC:DD:EE:02"

# Real packets from a BLU RC Button 4 (four button slots)...
PRESS_B1 = "44001f01643a013a003a003a00"   # id 31, button 1 press
DOUBLE_B1 = "44002001643a023a003a003a00"  # id 32, button 1 double press
PRESS_B2 = "44002101643a003a013a003a00"   # id 33, button 2 press
HOLD_B4 = "44002701643a003a003a003a80"    # id 39, button 4 hold
# ...and from a BLU Remote Control (two button slots, wheel, channel).
RC_B1_CH0 = "44005901643a013a006000"      # id 89, button 1, channel 0
RC_B1_CH1 = "44005f01643a013a006001"      # id 95, button 1, channel 1
RC_WHEEL_LEFT_START = "44005b01643c01006000"   # id 91, left, 0 steps, channel 0
RC_WHEEL_LEFT_FINAL = "44005c01643c01026000"   # id 92, left, 2 steps
RC_WHEEL_RIGHT_START = "44005d01643c02006000"  # id 93, right, 0 steps


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


def make_binding(binding_id: str, trigger_input: str, event: str, button: str = "desk",
                 channel: Optional[int] = None, cooldown_ms: int = 700,
                 enabled: bool = True) -> Binding:
    return Binding(binding_id, binding_id, Trigger(button, trigger_input, event, channel),
                   {"type": "http", "url": f"http://example.invalid/{binding_id}"},
                   enabled=enabled, cooldown_ms=cooldown_ms)


def fired_ids(result) -> list[str]:
    return [f.binding.id for f in result.fired]


class EngineTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.engine = TriggerEngine(clock=self.clock)
        self.buttons = [Button("desk", "Desk remote", DESK, "SBBT-104CUS"),
                        Button("other", "Other remote", OTHER)]

    def configure(self, *bindings: Binding) -> None:
        self.engine.configure(self.buttons, bindings)

    def press(self, payload: str, mac: str = DESK, rssi: int = -40):
        return self.engine.process(mac, rssi, bytes.fromhex(payload))


class DedupeTest(EngineTestCase):
    def test_repeats_of_one_event_fire_once(self) -> None:
        self.configure(make_binding("wake", "button:1", "press"))
        first = self.press(PRESS_B1)
        self.assertEqual(fired_ids(first), ["wake"])
        for _ in range(20):
            self.clock.advance(0.05)
            again = self.press(PRESS_B1)
            self.assertTrue(again.duplicate)
            self.assertEqual(again.fired, [])

    def test_new_packet_id_is_a_new_event(self) -> None:
        self.configure(make_binding("wake", "button:1", "press", cooldown_ms=0))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["wake"])
        self.clock.advance(2)
        # Same button and event, but a new packet id (here a different captured packet).
        self.assertEqual(fired_ids(self.press("44002f01643a013a003a003a00")), ["wake"])

    def test_same_packet_id_after_the_window_is_new(self) -> None:
        self.configure(make_binding("wake", "button:1", "press", cooldown_ms=0))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["wake"])
        self.clock.advance(TriggerEngine.DEDUPE_WINDOW_S + 1)
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["wake"])

    def test_burst_longer_than_the_window_stays_one_event(self) -> None:
        self.configure(make_binding("wake", "button:1", "press", cooldown_ms=0))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["wake"])
        for _ in range(8):
            self.clock.advance(2)
            self.assertTrue(self.press(PRESS_B1).duplicate)

    def test_dedupe_is_per_device(self) -> None:
        self.configure(make_binding("a", "button:1", "press", button="desk"),
                       make_binding("b", "button:1", "press", button="other"))
        self.assertEqual(fired_ids(self.press(PRESS_B1, mac=DESK)), ["a"])
        # The same packet id from another device is a different event.
        self.assertEqual(fired_ids(self.press(PRESS_B1, mac=OTHER)), ["b"])

    def test_tracked_devices_are_bounded(self) -> None:
        self.configure()
        for i in range(TriggerEngine.MAX_TRACKED_DEVICES + 50):
            self.press(PRESS_B1, mac=f"AA:BB:CC:00:{i // 256:02X}:{i % 256:02X}")
        self.assertLessEqual(len(self.engine._seen), TriggerEngine.MAX_TRACKED_DEVICES)


class MatchingTest(EngineTestCase):
    def test_button_and_event_must_match(self) -> None:
        self.configure(make_binding("b1press", "button:1", "press"),
                       make_binding("b1double", "button:1", "double_press"),
                       make_binding("b2press", "button:2", "press"),
                       make_binding("b4hold", "button:4", "hold_press"))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["b1press"])
        self.assertEqual(fired_ids(self.press(DOUBLE_B1)), ["b1double"])
        self.assertEqual(fired_ids(self.press(PRESS_B2)), ["b2press"])
        self.assertEqual(fired_ids(self.press(HOLD_B4)), ["b4hold"])

    def test_nothing_fires_without_a_matching_binding(self) -> None:
        self.configure(make_binding("b2press", "button:2", "press"))
        result = self.press(PRESS_B1)
        self.assertEqual(result.fired, [])
        self.assertEqual(len(result.observed), 1)

    def test_several_bindings_on_one_trigger_all_fire(self) -> None:
        self.configure(make_binding("one", "button:1", "press"),
                       make_binding("two", "button:1", "press"))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["one", "two"])

    def test_disabled_binding_does_not_fire(self) -> None:
        self.configure(make_binding("off", "button:1", "press", enabled=False),
                       make_binding("on", "button:1", "press"))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["on"])

    def test_binding_for_a_deleted_button_is_ignored(self) -> None:
        self.configure(make_binding("orphan", "button:1", "press", button="gone"))
        self.assertEqual(self.press(PRESS_B1).fired, [])

    def test_unknown_device_is_observed_but_fires_nothing(self) -> None:
        self.configure(make_binding("wake", "button:1", "press"))
        result = self.press(PRESS_B1, mac="11:22:33:44:55:66")
        self.assertFalse(result.known_device)
        self.assertEqual(len(result.observed), 1)
        self.assertEqual(result.fired, [])

    def test_channel_specific_and_any_channel(self) -> None:
        self.configure(make_binding("ch1", "button:1", "press", channel=1),
                       make_binding("any", "button:1", "press", channel=None, cooldown_ms=0))
        self.assertEqual(fired_ids(self.press(RC_B1_CH0)), ["any"])
        self.assertEqual(fired_ids(self.press(RC_B1_CH1)), ["ch1", "any"])

    def test_channel_trigger_does_not_match_a_device_without_channel(self) -> None:
        self.configure(make_binding("ch0", "button:1", "press", channel=0))
        self.assertEqual(self.press(PRESS_B1).fired, [])

    def test_mac_comparison_is_case_insensitive(self) -> None:
        self.configure(make_binding("wake", "button:1", "press"))
        self.assertEqual(fired_ids(self.press(PRESS_B1, mac=DESK.lower())), ["wake"])

    def test_observed_event_carries_the_packet_details(self) -> None:
        self.configure()
        obs = self.press(RC_B1_CH1, rssi=-55).observed[0]
        self.assertEqual((obs.mac, obs.rssi, obs.channel, obs.input, obs.event, obs.packet_id,
                          obs.battery), (DESK, -55, 1, "button:1", "press", 95, 100))


class WheelTest(EngineTestCase):
    def test_turn_fires_on_the_start_packet_only(self) -> None:
        self.configure(make_binding("left", "wheel:left", "turn"))
        self.assertEqual(fired_ids(self.press(RC_WHEEL_LEFT_START)), ["left"])
        self.clock.advance(0.5)
        final = self.press(RC_WHEEL_LEFT_FINAL)
        self.assertEqual(final.fired, [])
        self.assertEqual(final.observed, [])
        self.assertTrue(any("final packet ignored" in n for n in final.notes))

    def test_direction_selects_the_binding(self) -> None:
        self.configure(make_binding("left", "wheel:left", "turn"),
                       make_binding("right", "wheel:right", "turn"))
        self.assertEqual(fired_ids(self.press(RC_WHEEL_RIGHT_START)), ["right"])

    def test_wiggle_burst_is_absorbed_by_the_cooldown(self) -> None:
        self.configure(make_binding("left", "wheel:left", "turn", cooldown_ms=700))
        # Start packets alternate directions with fresh ids, as measured during a fast wiggle.
        self.assertEqual(fired_ids(self.press(RC_WHEEL_LEFT_START)), ["left"])
        self.clock.advance(0.12)
        again = self.press("44008101643c01006000")  # id 129, left start
        self.assertEqual(again.fired, [])
        self.assertEqual([(b.id, why) for b, why in again.suppressed], [("left", "cooldown")])

    def test_cooldown_expires(self) -> None:
        self.configure(make_binding("left", "wheel:left", "turn", cooldown_ms=700))
        self.assertEqual(fired_ids(self.press(RC_WHEEL_LEFT_START)), ["left"])
        self.clock.advance(0.8)
        self.assertEqual(fired_ids(self.press("44008101643c01006000")), ["left"])


class CooldownTest(EngineTestCase):
    def test_zero_cooldown_fires_every_new_event(self) -> None:
        self.configure(make_binding("wake", "button:1", "press", cooldown_ms=0))
        self.assertEqual(fired_ids(self.press(PRESS_B1)), ["wake"])
        self.assertEqual(fired_ids(self.press("44002f01643a013a003a003a00")), ["wake"])

    def test_cooldown_is_per_binding(self) -> None:
        self.configure(make_binding("a", "button:1", "press", cooldown_ms=5000),
                       make_binding("b", "button:1", "press", cooldown_ms=0))
        self.press(PRESS_B1)
        self.clock.advance(1)
        result = self.press("44002f01643a013a003a003a00")
        self.assertEqual(fired_ids(result), ["b"])
        self.assertEqual([x[0].id for x in result.suppressed], ["a"])


class PacketProblemsTest(EngineTestCase):
    def test_encrypted_payload_is_reported(self) -> None:
        self.configure()
        result = self.press("45aabbcc")
        self.assertIn("encrypted", result.error or "")
        self.assertEqual(result.observed, [])

    def test_empty_payload_is_reported(self) -> None:
        self.configure()
        self.assertIsNotNone(self.engine.process(DESK, -40, b"").error)

    def test_other_bthome_version_is_reported(self) -> None:
        self.configure()
        self.assertIn("version", self.press("24001f01643a01").error or "")

    def test_invalid_mac_is_reported(self) -> None:
        self.configure()
        self.assertIsNotNone(self.engine.process("not-a-mac", -40, bytes.fromhex(PRESS_B1)).error)


class ModelsTest(unittest.TestCase):
    def test_normalize_mac(self) -> None:
        self.assertEqual(normalize_mac("aa:bb:cc:dd:ee:ff"), "AA:BB:CC:DD:EE:FF")
        self.assertEqual(normalize_mac("AA-BB-CC-DD-EE-FF"), "AA:BB:CC:DD:EE:FF")
        self.assertEqual(normalize_mac(" aabbccddeeff "), "AA:BB:CC:DD:EE:FF")

    def test_normalize_mac_rejects_garbage(self) -> None:
        for bad in ("", "AA:BB", "AA:BB:CC:DD:EE:GG", "AA:BB:CC:DD:EE:FF:00"):
            with self.assertRaises(ValueError, msg=bad):
                normalize_mac(bad)

    def test_button_normalizes_its_mac(self) -> None:
        self.assertEqual(Button("b", "B", "aabbccddeeff").mac, "AA:BB:CC:DD:EE:FF")


if __name__ == "__main__":
    unittest.main()
