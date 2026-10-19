"""Unit tests for auth.py (password hashing, session tokens, login throttling).

Run from the project directory: python3 -m unittest discover -p '*_test.py'
"""

import os
import stat
import tempfile
import unittest
from pathlib import Path

from button_dispatcher import auth


class Clock:
    def __init__(self, now: float = 1_000_000.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now


class PasswordHashTest(unittest.TestCase):
    def test_correct_and_wrong_password(self) -> None:
        stored = auth.hash_password("correct horse")
        self.assertTrue(auth.verify_password_hash("correct horse", stored))
        self.assertFalse(auth.verify_password_hash("wrong horse", stored))

    def test_salt_makes_hashes_differ(self) -> None:
        self.assertNotEqual(auth.hash_password("same password"), auth.hash_password("same password"))

    def test_malformed_stored_values_never_verify(self) -> None:
        for stored in ("", "x", "scrypt$1$2", "md5$1$2$3$ab$cd", "scrypt$16384$8$1$zz$zz"):
            self.assertFalse(auth.verify_password_hash("anything", stored), stored)


class AuthStoreTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "auth.json"
        self.clock = Clock()
        self.store = auth.AuthStore(self.path, clock=self.clock)
        self.store.load()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_starts_unconfigured(self) -> None:
        self.assertFalse(self.store.is_configured)
        self.assertFalse(self.store.verify_password("anything goes"))
        self.assertFalse(self.store.verify_token(self.store.make_token()))

    def test_set_and_verify_password(self) -> None:
        self.store.set_password("a good password")
        self.assertTrue(self.store.is_configured)
        self.assertTrue(self.store.verify_password("a good password"))
        self.assertFalse(self.store.verify_password("a bad password"))

    def test_password_length_rules(self) -> None:
        for bad in ("short", "x" * (auth.MAX_PASSWORD_LENGTH + 1)):
            with self.assertRaises(auth.PasswordError):
                self.store.set_password(bad)
        self.assertFalse(self.store.is_configured)

    def test_file_is_private_and_holds_no_plain_password(self) -> None:
        self.store.set_password("a good password")
        self.assertEqual(stat.S_IMODE(os.stat(self.path).st_mode), 0o600)
        self.assertNotIn("a good password", self.path.read_text())

    def test_password_and_sessions_survive_a_restart(self) -> None:
        self.store.set_password("a good password")
        token = self.store.make_token()
        restarted = auth.AuthStore(self.path, clock=self.clock)
        restarted.load()
        self.assertTrue(restarted.verify_password("a good password"))
        self.assertTrue(restarted.verify_token(token))

    def test_token_expires(self) -> None:
        self.store.set_password("a good password")
        token = self.store.make_token()
        self.clock.now += auth.SESSION_TTL_S - 1
        self.assertTrue(self.store.verify_token(token))
        self.clock.now += 2
        self.assertFalse(self.store.verify_token(token))

    def test_tampered_or_garbage_tokens_are_rejected(self) -> None:
        self.store.set_password("a good password")
        token = self.store.make_token()
        expiry, nonce, sig = token.split(".")
        for bad in (None, "", "a.b", "a.b.c", f"{int(expiry) + 999}.{nonce}.{sig}",
                    f"{expiry}.{nonce}.{sig[:-2]}xx", token + "x"):
            self.assertFalse(self.store.verify_token(bad), str(bad))

    def test_changing_the_password_ends_old_sessions(self) -> None:
        self.store.set_password("a good password")
        token = self.store.make_token()
        self.store.set_password("another password")
        self.assertFalse(self.store.verify_token(token))
        self.assertTrue(self.store.verify_token(self.store.make_token()))


class LoginLimiterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = Clock(0.0)
        self.limiter = auth.LoginLimiter(clock=self.clock)

    def fail(self, client: str, times: int) -> None:
        for _ in range(times):
            self.limiter.record_failure(client)
            self.clock.now += 1

    def test_allows_a_few_failures(self) -> None:
        self.fail("1.2.3.4", auth.LoginLimiter.MAX_FAILURES - 1)
        self.assertEqual(self.limiter.retry_after("1.2.3.4"), 0.0)

    def test_blocks_after_too_many_failures_then_recovers(self) -> None:
        self.fail("1.2.3.4", auth.LoginLimiter.MAX_FAILURES)
        self.assertGreater(self.limiter.retry_after("1.2.3.4"), 0.0)
        self.clock.now += auth.LoginLimiter.BLOCK_S + 1
        self.assertEqual(self.limiter.retry_after("1.2.3.4"), 0.0)

    def test_clients_are_independent(self) -> None:
        self.fail("1.2.3.4", auth.LoginLimiter.MAX_FAILURES)
        self.assertEqual(self.limiter.retry_after("5.6.7.8"), 0.0)

    def test_success_clears_the_failures(self) -> None:
        self.fail("1.2.3.4", auth.LoginLimiter.MAX_FAILURES - 1)
        self.limiter.record_success("1.2.3.4")
        self.fail("1.2.3.4", auth.LoginLimiter.MAX_FAILURES - 1)
        self.assertEqual(self.limiter.retry_after("1.2.3.4"), 0.0)

    def test_tracked_clients_are_bounded(self) -> None:
        for i in range(auth.LoginLimiter.MAX_CLIENTS + 40):
            self.limiter.record_failure(f"10.0.{i // 256}.{i % 256}")
        self.assertLessEqual(len(self.limiter._failures), auth.LoginLimiter.MAX_CLIENTS)


if __name__ == "__main__":
    unittest.main()
