"""Reference Python API edge for version-one CLI application-error envelopes.

Exception types represent stable host presentation categories. ``domain`` and
``code`` retain the application-owned failure identity; callers never parse text.
"""

from __future__ import annotations

from copy import deepcopy
from typing import Any


class HoroError(Exception):
    """An application error with the same owned payload exposed by other hosts."""

    def __init__(self, payload: dict[str, Any], exit_code: int) -> None:
        self.payload = deepcopy(payload)
        self.exit_code = exit_code
        self.domain = self.payload["domain"]
        self.code = self.payload["code"]
        self.severity = self.payload["severity"]
        self.message = self.payload["message"]
        self.diagnostics = self.payload["diagnostics"]
        self.metadata = self.payload["metadata"]
        self.cause = self.payload["cause"]
        super().__init__(self.message)


class HoroUsageError(HoroError):
    """CLI request syntax or option failure (exit 2)."""


class HoroValidationError(HoroError):
    """Application input validation failure (exit 3)."""


class HoroCapabilityError(HoroError):
    """Required capability unavailable (exit 4)."""


class HoroOperationError(HoroError):
    """Application operation failure (exit 5)."""


class HoroPermissionError(HoroError):
    """Security or permission refusal (exit 6)."""


class HoroCancelledError(HoroError):
    """Cooperative cancellation (exit 7)."""


class HoroTimeoutError(HoroError):
    """Declared deadline elapsed (exit 8)."""


class HoroInvariantError(HoroError):
    """Reported in-process invariant failure (exit 10)."""


_EXCEPTIONS: dict[int, type[HoroError]] = {
    2: HoroUsageError,
    3: HoroValidationError,
    4: HoroCapabilityError,
    5: HoroOperationError,
    6: HoroPermissionError,
    7: HoroCancelledError,
    8: HoroTimeoutError,
    10: HoroInvariantError,
}
_SEVERITIES = {"info", "warning", "error", "fatal"}


def _is_integer(value: Any) -> bool:
    """Accept integer schema values while rejecting JSON booleans."""
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_payload(payload: Any, depth: int = 0) -> None:
    """Reject malformed or unbounded envelopes without inventing a business error."""
    if depth >= 16 or not isinstance(payload, dict):
        raise ValueError("Invalid Horo error payload")
    if set(payload) != {"domain", "code", "severity", "message", "diagnostics", "metadata", "cause"}:
        raise ValueError("Invalid Horo error fields")
    for key in ("domain", "code", "severity", "message"):
        value = payload[key]
        if not isinstance(value, str) or len(value.encode("utf-8")) > 4096:
            raise ValueError("Invalid Horo error text")
    if not payload["domain"] or not payload["code"] or payload["severity"] not in _SEVERITIES:
        raise ValueError("Invalid Horo error identity or severity")
    diagnostics = payload["diagnostics"]
    if not isinstance(diagnostics, list) or len(diagnostics) > 64 or payload["metadata"] != {}:
        raise ValueError("Invalid Horo error details")
    for diagnostic in diagnostics:
        _validate_diagnostic(diagnostic)
    if payload["cause"] is not None:
        _validate_payload(payload["cause"], depth + 1)


def _validate_diagnostic(diagnostic: Any) -> None:
    """Validate the canonical diagnostic vocabulary and location representation."""
    if not isinstance(diagnostic, dict) or set(diagnostic) != {"code", "severity", "message", "location", "path"}:
        raise ValueError("Invalid Horo diagnostic")
    for key in ("code", "severity", "message", "path"):
        value = diagnostic[key]
        if not isinstance(value, str) or len(value.encode("utf-8")) > 4096:
            raise ValueError("Invalid Horo diagnostic text")
    if not diagnostic["code"] or diagnostic["severity"] not in {"note", "warning", "error", "fatal"}:
        raise ValueError("Invalid Horo diagnostic identity or severity")
    location = diagnostic["location"]
    if not isinstance(location, dict) or set(location) != {"source", "line", "column"}:
        raise ValueError("Invalid Horo diagnostic location")
    if not isinstance(location["source"], str) or len(location["source"].encode("utf-8")) > 4096:
        raise ValueError("Invalid Horo diagnostic source")
    if any(not _is_integer(location[key]) or not 0 <= location[key] <= 0xFFFFFFFF for key in ("line", "column")):
        raise ValueError("Invalid Horo diagnostic coordinates")


def raise_application_error(envelope: dict[str, Any]) -> None:
    """Raise a typed exception only at the Python edge of a translated C++ failure.

    Malformed protocol/schema input raises ``ValueError``; it is never classified
    as a new application error. Host failure exit 1 and OS signals are excluded.
    """
    if not isinstance(envelope, dict) or set(envelope) != {"schemaVersion", "exitCode", "error"}:
        raise ValueError("Invalid Horo application-error envelope")
    if not _is_integer(envelope["schemaVersion"]) or envelope["schemaVersion"] != 1:
        raise ValueError("Unsupported Horo application-error schema")
    exit_code = envelope["exitCode"]
    if not _is_integer(exit_code) or exit_code not in _EXCEPTIONS:
        raise ValueError("Invalid Horo application-error exit category")
    _validate_payload(envelope["error"])
    raise _EXCEPTIONS[exit_code](envelope["error"], exit_code)
