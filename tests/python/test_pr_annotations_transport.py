"""Offline security, credential and bounded GitHub transport regressions."""

from __future__ import annotations

import argparse
import asyncio
import http.client
import json
from unittest.mock import AsyncMock, Mock
from urllib.parse import parse_qs, urlsplit

import pytest

from pr_annotations_test_support import annotations


@pytest.mark.parametrize(
    "value",
    [
        "--method=DELETE",
        "https://github.com/o/r",
        "o/../r",
        "o/r?x=1",
        "o/r\n",
        "ö/r",
        "o//r",
        "o/r#1",
    ],
)
def test_repository_rejects_options_paths_and_control_characters(value):
    with pytest.raises(argparse.ArgumentTypeError):
        annotations.repository(value)


def test_repository_accepts_owner_and_repo():
    assert annotations.repository("Org_Name/repo.name-1") == "Org_Name/repo.name-1"


@pytest.fixture
def transport(monkeypatch):
    response = Mock(status=200)
    response.read.return_value = b'{"ok":true}'
    connection = Mock()
    connection.getresponse.return_value = response
    factory = Mock(return_value=connection)
    monkeypatch.setattr(annotations.http.client, "HTTPSConnection", factory)
    return factory, connection, response


def test_https_fixed_host_json_and_connection_cleanup(transport):
    factory, connection, response = transport
    api = annotations.GitHub("fixture-token")
    assert api.request("POST", "/graphql", {"value": "--method=DELETE"}) == {"ok": True}
    factory.assert_called_once_with("api.github.com", timeout=30)
    args, kwargs = connection.request.call_args
    assert args == ("POST", "/graphql")
    assert json.loads(kwargs["body"]) == {"value": "--method=DELETE"}
    assert kwargs["headers"]["Authorization"] == "Bearer fixture-token"
    response.read.assert_called_once_with(annotations.MAX_RESPONSE_BYTES + 1)
    connection.close.assert_called_once_with()


@pytest.mark.parametrize("status", [302, 403, 500])
def test_http_errors_do_not_follow_redirects_or_expose_tokens(transport, status):
    factory, connection, response = transport
    response.status = status
    api = annotations.GitHub("secret-fixture")
    with pytest.raises(annotations.AnnotationError, match=f"HTTP {status}") as failure:
        api.request("GET", "/repos/o/r")
    assert "secret-fixture" not in str(failure.value)
    response.read.assert_not_called()
    assert factory.call_count == 1
    connection.close.assert_called_once_with()


@pytest.mark.parametrize("body, message", [(b"not-json", "JSON response"), (b"x" * 9, "exceeds")])
def test_bad_or_oversized_response_is_rejected(monkeypatch, transport, body, message):
    _, connection, response = transport
    monkeypatch.setattr(annotations, "MAX_RESPONSE_BYTES", 8)
    response.read.return_value = body
    api = annotations.GitHub("token")
    with pytest.raises(annotations.AnnotationError, match=message):
        api.request("GET", "/repos/o/r")
    connection.close.assert_called_once_with()


@pytest.mark.parametrize("error", [OSError("offline"), http.client.HTTPException("bad response")])
def test_transport_errors_close_connections(transport, error):
    _, connection, _ = transport
    connection.request.side_effect = error
    api = annotations.GitHub("token")
    with pytest.raises(annotations.AnnotationError, match="JSON response"):
        api.request("GET", "/repos/o/r")
    connection.close.assert_called_once_with()


def test_empty_response_is_allowed(transport):
    _, _, response = transport
    response.read.return_value = b""
    assert annotations.GitHub("token").request("DELETE", "/item") is None


@pytest.mark.parametrize("result", [None, [], {"errors": ["denied"]}, {"data": None}, {"data": []}])
def test_graphql_rejects_error_responses(monkeypatch, result):
    api = annotations.GitHub("token")
    monkeypatch.setattr(api, "request", Mock(return_value=result))
    with pytest.raises(annotations.AnnotationError, match="GraphQL request failed"):
        api.graphql("query", {})


def test_graphql_preserves_variables(monkeypatch):
    api = annotations.GitHub("token")
    request = Mock(return_value={"data": {"node": "result"}})
    monkeypatch.setattr(api, "request", request)
    assert api.graphql("query", {"id": '"payload'}) == {"node": "result"}
    request.assert_called_once_with(
        "POST", "/graphql", {"query": "query", "variables": {"id": '"payload'}}
    )


def test_rest_pagination_encodes_parameters_and_keeps_every_item(monkeypatch):
    api = annotations.GitHub("token")
    request = Mock(side_effect=[[{"number": n} for n in range(100)], [{"number": 100}]])
    monkeypatch.setattr(api, "request", request)
    assert len(api.pages("/pulls", {"state": "all & extra"})) == 101
    assert parse_qs(urlsplit(request.call_args.args[1]).query) == {
        "state": ["all & extra"],
        "per_page": ["100"],
        "page": ["2"],
    }


@pytest.mark.parametrize(
    "response, message", [({}, "must be an array"), ([{}] * 100, "1000 pages")]
)
def test_rest_pagination_is_bounded(monkeypatch, response, message):
    api = annotations.GitHub("token")
    monkeypatch.setattr(api, "request", Mock(return_value=response))
    with pytest.raises(annotations.AnnotationError, match=message):
        api.pages("/pulls")


@pytest.fixture
def no_environment_token(monkeypatch):
    monkeypatch.delenv("GH_TOKEN", raising=False)
    monkeypatch.delenv("GITHUB_TOKEN", raising=False)


def test_environment_credentials_have_precedence(monkeypatch, no_environment_token):
    monkeypatch.setenv("GITHUB_TOKEN", "fallback-fixture")
    assert annotations.github_token() == "fallback-fixture"
    monkeypatch.setenv("GH_TOKEN", "preferred-fixture")
    assert annotations.github_token() == "preferred-fixture"


@pytest.mark.parametrize("token", ["", "line\nbreak", "with space", "ö", "x\x7f"])
def test_malformed_credentials_are_rejected(monkeypatch, no_environment_token, token):
    monkeypatch.setattr(annotations, "cli_token", AsyncMock(return_value=token))
    with pytest.raises(annotations.AnnotationError, match="malformed"):
        annotations.github_token()


@pytest.mark.parametrize("error", [OSError("missing gh"), UnicodeError("bad encoding")])
def test_credential_lookup_failure_is_safe(monkeypatch, no_environment_token, error):
    monkeypatch.setattr(annotations, "cli_token", AsyncMock(side_effect=error))
    with pytest.raises(annotations.AnnotationError, match="cannot read GitHub CLI credentials"):
        annotations.github_token()


@pytest.mark.parametrize("returncode", [0, 1])
def test_cli_credential_arguments_are_fixed(monkeypatch, returncode):
    process = Mock(
        returncode=returncode,
        communicate=AsyncMock(return_value=(b"fixture-token\n", None)),
    )
    spawn = AsyncMock(return_value=process)
    monkeypatch.setattr(annotations.asyncio, "create_subprocess_exec", spawn)
    credential_lookup = annotations.cli_token()
    if returncode:
        with pytest.raises(annotations.AnnotationError, match="authenticate gh"):
            asyncio.run(credential_lookup)
    else:
        assert asyncio.run(credential_lookup) == "fixture-token"
    spawn.assert_awaited_once_with(
        "gh",
        "auth",
        "token",
        "--hostname",
        "github.com",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )


def test_cli_timeout_kills_and_reaps_process(monkeypatch):
    process = Mock(returncode=0, communicate=AsyncMock(side_effect=[TimeoutError(), (b"", None)]))
    monkeypatch.setattr(
        annotations.asyncio, "create_subprocess_exec", AsyncMock(return_value=process)
    )
    credential_lookup = annotations.cli_token()
    with pytest.raises(annotations.AnnotationError, match="timed out"):
        asyncio.run(credential_lookup)
    process.kill.assert_called_once_with()
    assert process.communicate.await_count == 2
