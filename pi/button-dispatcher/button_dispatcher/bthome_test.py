"""Unit tests for bthome.py, using packets captured from real Shelly BLU remotes.

Run from the project directory: python3 -m unittest discover -p '*_test.py'
"""

import unittest

from button_dispatcher import bthome


def decode(hex_payload: str) -> bthome.Packet:
    return bthome.decode(bytes.fromhex(hex_payload))


def events(hex_payload: str) -> list[tuple[str, str, int]]:
    return [(e.input, e.event, e.steps) for e in bthome.input_events(decode(hex_payload))]


class FourButtonRemoteTest(unittest.TestCase):
    """BLU RC Button 4 (advertises as SBBT-104CUS): four button slots per packet."""

    def test_press_on_button_1(self) -> None:
        p = decode("44001f01643a013a003a003a00")
        self.assertEqual(p.version, 2)
        self.assertFalse(p.encrypted)
        self.assertTrue(p.trigger_based)
        self.assertEqual(p.packet_id, 31)
        self.assertEqual(p.battery, 100)
        self.assertIsNone(p.channel)
        self.assertEqual(p.buttons, [1, 0, 0, 0])
        self.assertTrue(p.complete)

    def test_event_codes(self) -> None:
        self.assertEqual(events("44001f01643a013a003a003a00"), [("button:1", "press", 0)])
        self.assertEqual(events("44002001643a023a003a003a00"), [("button:1", "double_press", 0)])
        self.assertEqual(events("44002101643a003a033a003a00"), [("button:2", "triple_press", 0)])
        self.assertEqual(events("44002301643a003a003a003a03"), [("button:4", "triple_press", 0)])
        self.assertEqual(events("44002401643a803a003a003a00"), [("button:1", "hold_press", 0)])

    def test_no_event_in_any_slot(self) -> None:
        self.assertEqual(events("44001f01643a003a003a003a00"), [])

    def test_two_buttons_at_once(self) -> None:
        self.assertEqual(events("44001f01643a013a013a003a00"),
                         [("button:1", "press", 0), ("button:2", "press", 0)])

    def test_unknown_event_code_gets_a_name(self) -> None:
        self.assertEqual(events("44001f01643a7f3a003a003a00"), [("button:1", "event_0x7f", 0)])


class RemoteControlTest(unittest.TestCase):
    """BLU Remote Control ZB (SBRC-005B): two button slots, a wheel, and a channel."""

    def test_button_press_with_channel(self) -> None:
        p = decode("44005901643a013a006000")
        self.assertEqual(p.packet_id, 89)
        self.assertEqual(p.channel, 0)
        self.assertEqual(p.buttons, [1, 0])
        self.assertEqual(events("44005901643a013a006000"), [("button:1", "press", 0)])
        self.assertEqual(events("44006001643a003a016001"), [("button:2", "press", 0)])
        self.assertEqual(decode("44006001643a003a016001").channel, 1)

    def test_wheel_start_and_final_packets(self) -> None:
        # Start packet: direction with 0 steps. Final packet: direction with the step count.
        self.assertEqual(events("44005b01643c01006000"), [("wheel:left", "turn", 0)])
        self.assertEqual(events("44005c01643c01026000"), [("wheel:left", "turn", 2)])
        self.assertEqual(events("44006f01643c02006003"), [("wheel:right", "turn", 0)])
        self.assertEqual(events("44007001643c02016003"), [("wheel:right", "turn", 1)])

    def test_wheel_step_count_is_one_byte(self) -> None:
        p = decode("44007201643c017c6003")
        self.assertEqual(p.wheel_event, 1)
        self.assertEqual(p.wheel_steps, 124)
        self.assertEqual(p.channel, 3)
        self.assertEqual(decode("44007401643c02986003").wheel_steps, 152)

    def test_wheel_event_none_is_not_an_input(self) -> None:
        self.assertEqual(events("44005b01643c00006000"), [])


class RobustnessTest(unittest.TestCase):
    def test_empty_payload_raises(self) -> None:
        with self.assertRaises(ValueError):
            bthome.decode(b"")

    def test_encrypted_payload_is_not_parsed(self) -> None:
        p = decode("45aabbcc")
        self.assertTrue(p.encrypted)
        self.assertEqual(p.buttons, [])
        self.assertIsNone(p.packet_id)

    def test_unknown_object_stops_decoding(self) -> None:
        p = decode("44001f01643a01ff1122")
        self.assertFalse(p.complete)
        # What was decoded before the unknown object is kept.
        self.assertEqual(p.packet_id, 31)
        self.assertEqual(p.buttons, [1])

    def test_truncated_object_stops_decoding(self) -> None:
        p = decode("44001f3c01")
        self.assertFalse(p.complete)
        self.assertEqual(p.packet_id, 31)
        self.assertIsNone(p.wheel_event)

    def test_known_objects_of_other_devices_are_skipped_by_length(self) -> None:
        # A temperature object (id 0x02, 2 bytes) before the button event must not break parsing.
        p = decode("44001f02e8033a01")
        self.assertTrue(p.complete)
        self.assertEqual(p.buttons, [1])

    def test_other_version_is_reported(self) -> None:
        self.assertEqual(decode("24001f").version, 1)


if __name__ == "__main__":
    unittest.main()
