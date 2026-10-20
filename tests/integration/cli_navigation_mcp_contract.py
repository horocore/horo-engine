"""Exercise native project-bound navigation tools without granting protocol-controlled write authority."""
import json
import sys
import time
import unittest

from cli_mcp_stdio_contract import NativeCli, exchange


def terminal(process, checks, identity, reply):
    """Observe controller-owned completion without advancing the navigation application."""
    operation = reply["result"]
    deadline = time.monotonic() + 5
    while operation["status"] in ("queued", "running"):
        checks.assertLess(time.monotonic(), deadline, operation)
        time.sleep(0.01)
        identity += 1
        operation = exchange(process, checks, identity, "operations/get", {"operationId": operation["operationId"]})["result"]
    checks.assertEqual(operation["status"], "failed", operation)
    return operation["error"]["code"]


def run(executable, project, definition):
    """Inspect real process registration, strict input and normal host approval barriers."""
    cli = NativeCli(executable)
    checks = unittest.TestCase()
    with cli.start(["mcp", "serve", "--project", project, "--definition", definition]) as process:
        try:
            listed = exchange(process, checks, 1, "tools/list", {})
        except (json.JSONDecodeError, BrokenPipeError) as error:
            output, diagnostics = process.communicate(timeout=5)
            raise AssertionError((process.returncode, output, diagnostics)) from error
        names = {tool["name"] for tool in listed["result"]["tools"]}
        checks.assertEqual(names, {"observability.smoke", "navigation.bake", "navigation.operation", "navigation.cancel"})
        denied = exchange(process, checks, 2, "tools/call", {"name": "navigation.bake", "arguments": {}})
        checks.assertEqual(denied["error"]["data"]["code"], "mcp.approval_required", denied)
        invalid = exchange(process, checks, 3, "tools/call", {"name": "navigation.operation", "arguments": {"operationId": "1", "project": "foreign-root"}})
        checks.assertEqual(terminal(process, checks, 10, invalid), "mcp.tool_input_invalid", invalid)
        query = exchange(process, checks, 4, "tools/call", {"name": "navigation.operation", "arguments": {"operationId": "1"}})
        checks.assertEqual(terminal(process, checks, 1000, query), "navigation.handle.invalid", query)
        process.stdin.close()
        process.stdin = None
        output, diagnostics = process.communicate(timeout=15)
        checks.assertEqual((process.returncode, output), (0, b""), diagnostics)


if __name__ == "__main__":
    run(*sys.argv[1:4])
