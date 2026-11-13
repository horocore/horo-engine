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
    def start(self, arguments, *, windows_console=False):
        startup = None
        flags = 0
        if os.name == "nt" and windows_console:
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            flags = subprocess.CREATE_NEW_CONSOLE
        with subprocess.Popen([self.binary, *arguments], shell=False, close_fds=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              startupinfo=startup, creationflags=flags) as process:
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
        checks.assertEqual(denied["error"]["data"]["code"], "mcp.approval_required", denied)
        unavailable = exchange(process, checks, 3, "tools/call", {"name": "renderer.inspect", "arguments": {}})
        checks.assertEqual(unavailable["error"]["data"]["code"], "mcp.tool_unavailable", unavailable)
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
        check_windows_interrupts(cli, checks)
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


def windows_kernel():
    """Describe only the native APIs needed to signal an exact owned hidden console."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    file_time = ctypes.POINTER(wintypes.FILETIME)
    signatures = {
        "GetProcessTimes": ([wintypes.HANDLE, file_time, file_time, file_time, file_time], wintypes.BOOL),
        "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
        "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        "AttachConsole": ([wintypes.DWORD], wintypes.BOOL),
        "FreeConsole": ([], wintypes.BOOL),
        "SetConsoleCtrlHandler": ([ctypes.c_void_p, wintypes.BOOL], wintypes.BOOL),
        "GenerateConsoleCtrlEvent": ([wintypes.DWORD, wintypes.DWORD], wintypes.BOOL),
        "GetConsoleProcessList": ([ctypes.POINTER(wintypes.DWORD), wintypes.DWORD], wintypes.DWORD),
        "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes = arguments
        function.restype = result
    return kernel


def windows_check(success):
    """Retain the native error immediately after a failed API call."""
    if not success:
        import ctypes
        raise ctypes.WinError(ctypes.get_last_error())


def windows_birth(kernel, handle):
    """Read creation identity from an owned process handle, without reopening its PID."""
    import ctypes
    from ctypes import wintypes
    creation, exit_time, system, user = (wintypes.FILETIME() for _ in range(4))
    windows_check(kernel.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                         ctypes.byref(system), ctypes.byref(user)))
    return (creation.dwHighDateTime << 32) | creation.dwLowDateTime


def signal_owned_windows_console(pid, birth, event):
    """Broadcast only after verifying the live child identity and its two-client private console."""
    import ctypes
    from ctypes import wintypes
    if event not in (0, 1):
        raise ValueError("Only CTRL_C and CTRL_BREAK are qualified")
    kernel = windows_kernel()
    process = kernel.OpenProcess(0x00100000 | 0x1000, False, pid)
    windows_check(process)
    attached = False
    observed = threading.Event()
    handler_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)

    def absorb(kind):
        if kind == event:
            observed.set()
        return True

    handler = handler_type(absorb)
    try:
        if windows_birth(kernel, process) != birth or kernel.WaitForSingleObject(process, 0) != 258:
            raise RuntimeError("Owned CLI process identity is no longer live")
        windows_check(kernel.AttachConsole(pid))
        attached = True
        windows_check(kernel.SetConsoleCtrlHandler(None, False))
        windows_check(kernel.SetConsoleCtrlHandler(ctypes.cast(handler, ctypes.c_void_p), True))
        clients = (wintypes.DWORD * 8)()
        count = kernel.GetConsoleProcessList(clients, len(clients))
        if count != 2 or set(clients[:count]) != {pid, os.getpid()}:
            raise RuntimeError("Console contains an unowned process; interrupt refused")
        windows_check(kernel.GenerateConsoleCtrlEvent(event, 0))
        if not observed.wait(5):
            raise RuntimeError("Private console did not acknowledge its control event")
    finally:
        if attached:
            kernel.FreeConsole()
        kernel.CloseHandle(process)


def check_windows_interrupts(cli, checks):
    """Use an isolated hidden console without modifying the test driver's console or handlers."""
    for event in (0, 1):
        with cli.start(["mcp", "serve"], windows_console=True) as process:
            exchange(process, checks, 1, "tools/list", {})
            birth = windows_birth(windows_kernel(), int(process._handle))
            process.stdin.write(b"{partial")
            process.stdin.flush()
            helper = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--signal-owned-console",
                                     str(process.pid), str(birth), str(event)], close_fds=True,
                                    creationflags=subprocess.CREATE_NO_WINDOW, capture_output=True, timeout=10)
            checks.assertEqual(helper.returncode, 0, helper.stderr)
            process.wait(timeout=15)
            output, diagnostics = process.communicate(timeout=5)
            checks.assertEqual((process.returncode, output), (7, b""), diagnostics)


def check_timeout(cli, checks):
    """Observe a native deadline with stdin held open and no protocol frames."""
    with cli.start(["mcp", "serve", "--timeout-ms", "150", "--output", "json"]) as process:
        process.wait(timeout=10)
        output, diagnostics = process.communicate(timeout=5)
        checks.assertEqual((process.returncode, output), (8, b""), diagnostics)
        checks.assertIn(b"cli.execution_timed_out", diagnostics)
        fixture = json.loads((Path(__file__).parents[1] / "fixtures/cli/output-v1.json").read_text(encoding="utf-8"))
        envelopes = [json.loads(line) for line in diagnostics.splitlines() if line.startswith(b'{"command":')]
        checks.assertEqual(len(envelopes), 1, diagnostics)
        error = envelopes[0]
        contract = fixture["commandSchemas"][error["outputSchema"]["id"]]
        checks.assertEqual((error["command"], error["outputSchema"]["version"]), (contract["command"], contract["version"]))
        checks.assertEqual(error["schemaVersion"], fixture["schemaVersion"])
        checks.assertEqual(error["exitCode"], fixture["exitCategories"]["timeout"])


def run(executable):
    cli = NativeCli(executable)
    checks = unittest.TestCase()
    check_eof(cli, checks)
    check_terminal(cli, checks)
    check_authority(cli, checks)
    check_interrupts(cli, checks)
    check_timeout(cli, checks)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--signal-owned-console":
        signal_owned_windows_console(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
    else:
        run(sys.argv[1])
