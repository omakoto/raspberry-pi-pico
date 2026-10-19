"""Unit tests for learn.py (the "Find button" state machine).

Run from the project directory: python3 -m unittest discover -p '*_test.py'
"""

import unittest
from typing import Optional

from button_dispatcher.learn import LearnSession, describe
from button_dispatcher.triggers import Observed

DESK = "AA:BB:CC:DD:EE:01"
NEIGHBOR = "AA:BB:CC:DD:EE:99"


class Clock:
    def __init__(self) -> None:
        self.now = 100.0

    def __call__(self) -> float:
        return self.now


def obs(mac: str = DESK, rssi: Optional[int] = -40, channel: Optional[int] = None,
        input: str = "button:2", event: str = "press", packet_id: int = 1) -> Observed:
    return Observed(mac, rssi, channel, input, event, 0, packet_id, 100)


class DescribeTest(unittest.TestCase):
    def test_descriptions(self) -> None:
        self.assertEqual(describe(obs()), "button 2, single press")
        self.assertEqual(describe(obs(channel=3, event="hold_press", input="button:1")),
                         "button 1, hold, channel 3")
        self.assertEqual(describe(obs(input="wheel:left", event="turn", channel=0)),
                         "wheel turned left, channel 0")


class LearnSessionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = Clock()
        self.learn = LearnSession(clock=self.clock)

    def first_press(self, *observations: Observed) -> None:
        """Starts a session, feeds the given first presses and lets the collect window pass."""
        self.learn.start()
        for o in observations:
            self.learn.feed(o, name="SBBT-104CUS")
        self.clock.now += LearnSession.COLLECT_WINDOW_S + 0.1

    def test_idle_by_default(self) -> None:
        self.assertEqual(self.learn.status()["phase"], "idle")
        self.assertFalse(self.learn.active)

    def test_waits_for_the_first_press(self) -> None:
        self.learn.start()
        s = self.learn.status()
        self.assertEqual((s["phase"], s["candidate"]), ("waiting", None))
        self.assertTrue(self.learn.active)

    def test_first_press_becomes_the_candidate_after_the_collect_window(self) -> None:
        self.learn.start()
        self.learn.feed(obs(), name="SBBT-104CUS")
        self.assertEqual(self.learn.status()["phase"], "waiting")   # still collecting
        self.clock.now += LearnSession.COLLECT_WINDOW_S + 0.1
        s = self.learn.status()
        self.assertEqual(s["phase"], "confirm")
        self.assertEqual(s["candidate"]["mac"], DESK)
        self.assertEqual(s["candidate"]["name"], "SBBT-104CUS")
        self.assertEqual(s["candidate"]["description"], "button 2, single press")
        self.assertIn("once more", s["message"])

    def test_strongest_signal_wins_among_devices_pressed_together(self) -> None:
        self.first_press(obs(mac=NEIGHBOR, rssi=-85), obs(mac=DESK, rssi=-40))
        self.assertEqual(self.learn.status()["candidate"]["mac"], DESK)
        # Pressing order doesn't matter; the signal does.
        self.learn.start()
        self.learn.feed(obs(mac=DESK, rssi=-40))
        self.learn.feed(obs(mac=NEIGHBOR, rssi=-85))
        self.clock.now += 1
        self.assertEqual(self.learn.status()["candidate"]["mac"], DESK)

    def test_missing_signal_strength_loses(self) -> None:
        self.first_press(obs(mac=NEIGHBOR, rssi=None), obs(mac=DESK, rssi=-90))
        self.assertEqual(self.learn.status()["candidate"]["mac"], DESK)

    def test_second_identical_press_confirms(self) -> None:
        self.first_press(obs(packet_id=1))
        self.learn.feed(obs(packet_id=2))
        s = self.learn.status()
        self.assertEqual(s["phase"], "confirmed")
        self.assertEqual(s["candidate"]["mac"], DESK)
        self.assertFalse(self.learn.active)

    def test_other_device_cannot_confirm(self) -> None:
        self.first_press(obs())
        self.learn.feed(obs(mac=NEIGHBOR, rssi=-30))
        self.assertEqual(self.learn.status()["phase"], "confirm")

    def test_different_press_from_the_same_device_explains_the_mismatch(self) -> None:
        self.first_press(obs(input="button:2", event="press"))
        self.learn.feed(obs(input="button:3", event="press", packet_id=2))
        s = self.learn.status()
        self.assertEqual(s["phase"], "confirm")
        self.assertIn("button 3", s["message"])
        self.assertIn("button 2", s["message"])
        # A matching press afterwards still confirms, and clears the complaint.
        self.learn.feed(obs(input="button:2", event="press", packet_id=3))
        self.assertEqual(self.learn.status()["phase"], "confirmed")

    def test_channel_must_match_too(self) -> None:
        self.first_press(obs(channel=1))
        self.learn.feed(obs(channel=2, packet_id=2))
        self.assertEqual(self.learn.status()["phase"], "confirm")
        self.learn.feed(obs(channel=1, packet_id=3))
        self.assertEqual(self.learn.status()["phase"], "confirmed")

    def test_times_out_without_any_press(self) -> None:
        self.learn.start()
        self.clock.now += LearnSession.FIRST_PRESS_TIMEOUT_S + 1
        s = self.learn.status()
        self.assertEqual(s["phase"], "timeout")
        self.assertIn("No button was heard", s["message"])

    def test_times_out_when_not_confirmed(self) -> None:
        self.first_press(obs())
        self.clock.now += LearnSession.CONFIRM_TIMEOUT_S + 1
        s = self.learn.status()
        self.assertEqual(s["phase"], "timeout")
        self.assertIn("wasn't confirmed", s["message"])

    def test_press_after_timeout_is_ignored(self) -> None:
        self.first_press(obs())
        self.clock.now += LearnSession.CONFIRM_TIMEOUT_S + 1
        self.learn.feed(obs(packet_id=2))
        self.assertEqual(self.learn.status()["phase"], "timeout")

    def test_presses_are_ignored_when_not_searching(self) -> None:
        self.learn.feed(obs())
        self.assertEqual(self.learn.status()["phase"], "idle")

    def test_cancel_and_restart(self) -> None:
        self.first_press(obs())
        self.learn.cancel()
        self.assertEqual(self.learn.status()["phase"], "idle")
        self.learn.start()
        self.assertIsNone(self.learn.status()["candidate"])


if __name__ == "__main__":
    unittest.main()
