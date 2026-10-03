"""Python application-error edge contract, ownership and malformed-input tests."""

from __future__ import annotations

import pytest

from host_error_parity import EXCEPTIONS, errors, verify_cpp_parity


def envelope(exit_code: int = 5) -> dict:
    return {
        "schemaVersion": 1,
        "exitCode": exit_code,
        "error": {
            "domain": "project.example",
            "code": "project.example.failed",
            "severity": "error",
            "message": "Safe summary",
            "diagnostics": [],
            "metadata": {},
            "cause": None,
        },
    }


@pytest.mark.parametrize("exit_code,exception", EXCEPTIONS.items())
def test_python_exceptions_own_application_payload(exit_code: int, exception: type) -> None:
    source = envelope(exit_code)
    with pytest.raises(exception) as caught:
        errors.raise_application_error(source)
    failure = caught.value
    assert isinstance(failure, errors.HoroError)
    assert failure.payload == source["error"]
    assert failure.domain == "project.example"
    assert failure.code == "project.example.failed"
    assert failure.exit_code == exit_code
    source["error"]["message"] = "Changed source"
    source["error"]["diagnostics"].append({})
    assert str(failure) == "Safe summary"
    assert failure.diagnostics == []


@pytest.mark.parametrize("field,value", [("schemaVersion", 2), ("schemaVersion", True), ("exitCode", 1),
                                         ("exitCode", 0), ("exitCode", True), ("exitCode", 9)])
def test_invalid_envelope_is_protocol_failure(field: str, value: object) -> None:
    source = envelope()
    source[field] = value
    with pytest.raises(ValueError):
        errors.raise_application_error(source)


@pytest.mark.parametrize("field,value", [("code", ""), ("severity", "unknown"), ("message", "x" * 4097),
                                         ("diagnostics", [{}] * 65), ("metadata", []), ("cause", {})])
def test_invalid_payload_is_protocol_failure(field: str, value: object) -> None:
    source = envelope()
    source["error"][field] = value
    with pytest.raises(ValueError):
        errors.raise_application_error(source)


def test_depth_and_diagnostic_validation() -> None:
    source = envelope()
    node = source["error"]
    for _ in range(16):
        node["cause"] = envelope()["error"]
        node = node["cause"]
    with pytest.raises(ValueError):
        errors.raise_application_error(source)
    source = envelope()
    source["error"]["diagnostics"] = [{"code": "finding", "severity": "error", "message": "detail", "path": "field",
                                       "location": {"source": "project.json", "line": -1, "column": 0}}]
    with pytest.raises(ValueError):
        errors.raise_application_error(source)
    source["error"]["diagnostics"][0]["location"]["line"] = 4
    with pytest.raises(errors.HoroOperationError) as caught:
        errors.raise_application_error(source)
    assert caught.value.diagnostics[0]["path"] == "field"


@pytest.mark.parametrize("field,value", [("code", ""), ("code", 5), ("message", "x" * 4097),
                                         ("severity", "unknown"), ("location", []),
                                         ("location", {"source": "x", "line": 1}),
                                         ("location", {"source": 5, "line": 1, "column": 0}),
                                         ("location", {"source": "x", "line": True, "column": 0})])
def test_malformed_diagnostics_remain_protocol_failures(field: str, value: object) -> None:
    source = envelope()
    diagnostic = {"code": "finding", "severity": "warning", "message": "detail", "path": "field",
                  "location": {"source": "project.json", "line": 4, "column": 0}}
    diagnostic[field] = value
    source["error"]["diagnostics"] = [diagnostic]
    with pytest.raises(ValueError):
        errors.raise_application_error(source)


def test_unexpected_payload_fields_and_wrong_types() -> None:
    source = envelope()
    source["error"]["additional"] = "unsupported"
    with pytest.raises(ValueError):
        errors.raise_application_error(source)
    source = envelope()
    source["error"]["message"] = None
    with pytest.raises(ValueError):
        errors.raise_application_error(source)
    source = envelope()
    source["error"]["metadata"] = {"untyped": "unbounded"}
    with pytest.raises(ValueError):
        errors.raise_application_error(source)
    with pytest.raises(ValueError):
        errors.raise_application_error({})


def test_parity_runner_rejects_missing_categories() -> None:
    with pytest.raises(AssertionError):
        verify_cpp_parity([])


def test_parity_runner_rejects_host_payload_drift() -> None:
    cases = [{"cli": envelope(exit_code), "gui": {}} for exit_code in EXCEPTIONS]
    with pytest.raises(AssertionError):
        verify_cpp_parity(cases)
