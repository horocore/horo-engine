"""Exercise exclusive MCP stdio through the trusted CMake-built executable, with bounded waits."""
import contextlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import unittest


class NativeCli:
    """Own shell-free child execution of the exact production artifact supplied by CTest."""

    def __init__(self, executable):
        binary = Path(executable)
        expected_name = "horo-engine.exe" if os.name == "nt" else "horo-engine"
        if not binary.is_absolute() or binary.name != expected_name or not binary.is_file():
            raise ValueError("CTest must supply an absolute path to the built horo-engine executable")
        self.binary = str(binary.resolve(strict=True))

    @contextlib.contextmanager
    def start(self, arguments):
        with subprocess.Popen([self.binary, *arguments], shell=False, close_fds=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            watchdog = threading.Timer(25, process.kill)
            watchdog.start()
            try:
                yield process
            finally:
                # Failed assertions must also stop and drain the child before Popen.__exit__ waits.
                try:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=5)
                finally:
                    watchdog.cancel()

    def complete(self, arguments, request=b""):
        with self.start(arguments) as process:
            output, diagnostics = process.communicate(input=request, timeout=20)
            return process.returncode, output, diagnostics


def check_eof(cli, checks):
    for arguments in (["mcp", "serve"], ["mcp", "serve", "--output", "jsonl"]):
        request = b'{"jsonrpc":"2.0","id":17,"method":"tools/list"}\nnot-json\n{partial'
        code, output, diagnostics = cli.complete(arguments, request)
        checks.assertEqual(code, 0, diagnostics)
        frames = [json.loads(line) for line in output.splitlines()]
        checks.assertEqual(len(frames), 2, output)
        checks.assertEqual((frames[0]["id"], frames[0]["jsonrpc"]), (17, "2.0"))
        checks.assertEqual([tool["name"] for tool in frames[0]["result"]["tools"]], ["observability.smoke"])
        checks.assertEqual(frames[1]["error"]["code"], -32700)
        checks.assertTrue(all("invocationId" not in frame for frame in frames))


def exchange(process, checks, identity, method, params):
    request = {"jsonrpc": "2.0", "id": identity, "method": method, "params": params}
    process.stdin.write(json.dumps(request).encode() + b"\n")
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())
    checks.assertEqual((reply["jsonrpc"], reply["id"]), ("2.0", identity), reply)
    return reply


def check_authority(cli, checks):
    with cli.start(["mcp", "serve", "--output", "jsonl"]) as process:
        # A CLI capability and local authentication do not approve a mutating MCP request.
        denied = exchange(process, checks, 1, "tools/call", {"name": "observability.smoke", "arguments": {}})
        checks.assertEqual(denied["error"]["data"]["code"], "approval_required", denied)
        unavailable = exchange(process, checks, 3, "tools/call", {"name": "renderer.inspect", "arguments": {}})
        checks.assertEqual(unavailable["error"]["data"]["code"], "tool_unavailable", unavailable)
        process.stdin.close()
        process.stdin = None
        output, diagnostics = process.communicate(timeout=15)
        checks.assertEqual((process.returncode, output), (0, b""), diagnostics)


def check_terminal(cli, checks):
    code, output, diagnostics = cli.complete(["observability", "smoke", "--output", "json"])
    checks.assertEqual(code, 0, diagnostics)
    checks.assertEqual(json.loads(output)["result"], {"completed": True})
    code, output, diagnostics = cli.complete(["mcp", "serve", "--unknown", "--output", "json"])
    checks.assertEqual((code, output), (2, b""), diagnostics)


def check_interrupts(cli, checks):
    if os.name == "nt":
        return
    for stop in (signal.SIGINT, signal.SIGTERM):
        with cli.start(["mcp", "serve"]) as process:
            process.stdin.write(b'{"jsonrpc":"2.0","id":1,"method":"tools/list"}\n')
            process.stdin.flush()
            checks.assertEqual(json.loads(process.stdout.readline())["id"], 1)
            process.stdin.write(b"{partial")
            process.stdin.flush()
            process.send_signal(stop)
            # Leave stdin open until cancellation completes, proving it does not depend on EOF.
            process.wait(timeout=15)
            output, diagnostics = process.communicate(timeout=5)
            checks.assertEqual((process.returncode, output), (7, b""), diagnostics)


def run(executable):
    cli = NativeCli(executable)
    checks = unittest.TestCase()
    check_eof(cli, checks)
    check_terminal(cli, checks)
    check_authority(cli, checks)
    check_interrupts(cli, checks)


if __name__ == "__main__":
    run(sys.argv[1])
