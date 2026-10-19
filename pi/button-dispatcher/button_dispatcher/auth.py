"""Password storage, session tokens and login throttling for the web UI.

The password is kept as a salted scrypt hash. A session is a signed token (expiry, random nonce,
HMAC) in a cookie, so sessions survive a restart; changing the password rotates the signing key,
which ends every existing session.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import secrets
import time
from pathlib import Path
from typing import Callable, Optional

MIN_PASSWORD_LENGTH = 8
MAX_PASSWORD_LENGTH = 256
SESSION_TTL_S = 30 * 24 * 3600

_SCRYPT_N = 2 ** 14
_SCRYPT_R = 8
_SCRYPT_P = 1


def hash_password(password: str, salt: Optional[bytes] = None) -> str:
    salt = salt if salt is not None else secrets.token_bytes(16)
    digest = hashlib.scrypt(password.encode(), salt=salt, n=_SCRYPT_N, r=_SCRYPT_R, p=_SCRYPT_P,
                            dklen=32)
    return f"scrypt${_SCRYPT_N}${_SCRYPT_R}${_SCRYPT_P}${salt.hex()}${digest.hex()}"


def verify_password_hash(password: str, stored: str) -> bool:
    try:
        scheme, n, r, p, salt_hex, digest_hex = stored.split("$")
        if scheme != "scrypt":
            return False
        salt = bytes.fromhex(salt_hex)
        expected = bytes.fromhex(digest_hex)
        actual = hashlib.scrypt(password.encode(), salt=salt, n=int(n), r=int(r), p=int(p),
                                dklen=len(expected))
    except (ValueError, TypeError):
        return False
    return hmac.compare_digest(actual, expected)


class PasswordError(ValueError):
    """The password doesn't meet the rules."""


class AuthStore:
    """Persists the password hash and the session-signing key in auth.json (mode 0600)."""

    def __init__(self, path: Path, clock: Callable[[], float] = time.time) -> None:
        self.path = path
        self._clock = clock
        self._password_hash = ""
        self._key = b""

    def load(self) -> None:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            self._password_hash = str(data.get("password", ""))
            self._key = bytes.fromhex(str(data.get("key", "")))
        except FileNotFoundError:
            pass
        if not self._key:
            self._key = secrets.token_bytes(32)

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        text = json.dumps({"password": self._password_hash, "key": self._key.hex()}) + "\n"
        tmp = self.path.with_name(self.path.name + ".tmp")
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)

    @property
    def is_configured(self) -> bool:
        return bool(self._password_hash)

    def set_password(self, password: str) -> None:
        if not MIN_PASSWORD_LENGTH <= len(password) <= MAX_PASSWORD_LENGTH:
            raise PasswordError(f"password must be {MIN_PASSWORD_LENGTH}-{MAX_PASSWORD_LENGTH} characters")
        self._password_hash = hash_password(password)
        # A new key invalidates every session issued under the old password.
        self._key = secrets.token_bytes(32)
        self._save()

    def verify_password(self, password: str) -> bool:
        return self.is_configured and verify_password_hash(password, self._password_hash)

    def make_token(self) -> str:
        expiry = int(self._clock()) + SESSION_TTL_S
        payload = f"{expiry}.{secrets.token_hex(8)}"
        return f"{payload}.{self._sign(payload)}"

    def verify_token(self, token: Optional[str]) -> bool:
        if not token or not self.is_configured:
            return False
        try:
            expiry_text, nonce, signature = token.split(".")
            expiry = int(expiry_text)
        except ValueError:
            return False
        if not hmac.compare_digest(signature, self._sign(f"{expiry_text}.{nonce}")):
            return False
        return expiry > self._clock()

    def _sign(self, payload: str) -> str:
        digest = hmac.new(self._key, payload.encode(), hashlib.sha256).digest()
        return base64.urlsafe_b64encode(digest).decode().rstrip("=")


class LoginLimiter:
    """Blocks a client after repeated failed logins (guessing protection for a LAN service)."""

    MAX_FAILURES = 5
    WINDOW_S = 300.0
    BLOCK_S = 60.0
    MAX_CLIENTS = 256

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self._clock = clock
        self._failures: dict[str, list[float]] = {}

    def retry_after(self, client: str) -> float:
        """Seconds the client must wait before trying again (0 if it may try now)."""
        now = self._clock()
        recent = [t for t in self._failures.get(client, []) if now - t < self.WINDOW_S]
        if len(recent) < self.MAX_FAILURES:
            return 0.0
        return max(0.0, recent[-1] + self.BLOCK_S - now)

    def record_failure(self, client: str) -> None:
        now = self._clock()
        recent = [t for t in self._failures.get(client, []) if now - t < self.WINDOW_S]
        recent.append(now)
        self._failures[client] = recent
        if len(self._failures) > self.MAX_CLIENTS:
            oldest = min(self._failures, key=lambda c: self._failures[c][-1])
            del self._failures[oldest]

    def record_success(self, client: str) -> None:
        self._failures.pop(client, None)
