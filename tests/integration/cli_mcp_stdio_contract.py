"""Exercise exclusive MCP stdio through the production executable, with bounded waits."""
import json
import os
import signal
import subprocess
import sys
import threading


def run(executable):
    for arguments in (["mcp", "serve"], ["mcp", "serve", "--output", "jsonl"]):
        request = b'{"jsonrpc":"2.0","id":17,"method":"tools/list"}\nnot-json\n{partial'
        process = subprocess.run([executable, *arguments], input=request, capture_output=True, timeout=20, check=False)
        assert process.returncode == 0, process.stderr
        frames = [json.loads(line) for line in process.stdout.splitlines()]
        assert len(frames) == 2, process.stdout
        assert frames[0]["id"] == 17 and frames[0]["jsonrpc"] == "2.0"
        assert [tool["name"] for tool in frames[0]["result"]["tools"]] == ["observability.smoke"]
        assert frames[1]["error"]["code"] == -32700
        assert all("invocationId" not in frame for frame in frames)
    terminal = subprocess.run([executable, "observability", "smoke", "--output", "json"],
                              capture_output=True, timeout=20, check=False)
    assert terminal.returncode == 0, terminal.stderr
    assert json.loads(terminal.stdout)["result"] == {"completed": True}
    with subprocess.Popen([executable, "mcp", "serve", "--output", "jsonl"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        watchdog = threading.Timer(25, process.kill)
        watchdog.start()
        try:
            def exchange(identity, method, params):
                request = {"jsonrpc": "2.0", "id": identity, "method": method, "params": params}
                process.stdin.write(json.dumps(request).encode() + b"\n")
                process.stdin.flush()
                reply = json.loads(process.stdout.readline())
                assert reply["jsonrpc"] == "2.0" and reply["id"] == identity, reply
                return reply
            # A CLI capability and local authentication do not approve a mutating MCP request.
            denied = exchange(1, "tools/call", {"name": "observability.smoke", "arguments": {}})
            assert denied["error"]["data"]["code"] == "approval_required", denied
            unavailable = exchange(3, "tools/call", {"name": "renderer.inspect", "arguments": {}})
            assert unavailable["error"]["data"]["code"] == "tool_unavailable", unavailable
            process.stdin.close()
            process.stdin = None
            output, diagnostics = process.communicate(timeout=15)
            assert process.returncode == 0 and output == b"", (process.returncode, output, diagnostics)
        finally:
            watchdog.cancel()
    failed = subprocess.run([executable, "mcp", "serve", "--unknown", "--output", "json"],
                            input=b"", capture_output=True, timeout=20, check=False)
    assert failed.returncode == 2 and failed.stdout == b"", (failed.returncode, failed.stdout, failed.stderr)
    if os.name != "nt":
        for stop in (signal.SIGINT, signal.SIGTERM):
            with subprocess.Popen([executable, "mcp", "serve"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                process.stdin.write(b'{"jsonrpc":"2.0","id":1,"method":"tools/list"}\n')
                process.stdin.flush()
                # communicate is bounded; a watchdog kills a broken startup before readline can hang CTest.
                watchdog = threading.Timer(15, process.kill)
                watchdog.start()
                try:
                    assert json.loads(process.stdout.readline())["id"] == 1
                    process.stdin.write(b"{partial")
                    process.stdin.flush()
                    process.send_signal(stop)
                    # Leave stdin open until cancellation completes, proving it does not depend on EOF.
                    process.wait(timeout=15)
                    output, diagnostics = process.communicate(timeout=5)
                    assert process.returncode == 7 and output == b"", (process.returncode, output, diagnostics)
                finally:
                    watchdog.cancel()


if __name__ == "__main__":
    run(sys.argv[1])
