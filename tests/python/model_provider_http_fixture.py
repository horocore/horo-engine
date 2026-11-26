"""Exercise both production HTTP adapters against deterministic loopback wire protocols."""

import json
import os
import socket
import subprocess  # nosec B404 - this fixture executes only the validated CMake test target.
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            # The cancellation tests deliberately close the active connection.
            pass

    def send_records(self, records, content_type="application/json", status=200, retry_after=None):
        data = "".join(records).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        if retry_after is not None:
            self.send_header("Retry-After", retry_after)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in {"/api/tags", "/other/api/tags"}:
            self.send_records([json.dumps({"models": [{"name": "normal"}]})])
        elif self.path == "/v1/models" and self.headers.get("Authorization") == "Bearer fixture-token":
            self.send_records([json.dumps({"data": [{"id": "normal"}]})])
        else:
            self.send_records([json.dumps({"error": "private"})], status=401)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(length))
        if self.path == "/api/show":
            if self.headers.get("Authorization") is not None or payload not in ({"model": "editor-approved-tool"}, {"model": "normal"}):
                self.send_records(["{}"], status=400)
                return
            supports_tools = payload["model"] == "editor-approved-tool"
            self.server.approved_tool_probe = supports_tools
            self.send_records([json.dumps({"capabilities": ["completion", "tools"] if supports_tools else ["completion"]})])
            return
        cloud = self.path == "/v1/chat/completions"
        if not cloud and self.path != "/api/chat":
            self.send_records(["{}"], status=404)
            return
        if cloud and self.headers.get("Authorization") != "Bearer fixture-token":
            self.send_records([json.dumps({"error": "private"})], status=401)
            return
        if not cloud and payload["model"] in {"editor-local-tool", "editor-approved-tool"}:
            self.send_editor_local_tool(payload)
            return
        editor_chat = {
            "editor-dock-lifecycle": self.send_dock_lifecycle,
            "editor-project-lifecycle": self.send_project_lifecycle,
            "editor-inline-patch": self.send_inline_patch,
            "editor-multi-inline-patch": self.send_multi_inline_patch,
        }.get(payload["model"])
        if not cloud and editor_chat:
            editor_chat(payload)
            return
        if not cloud and payload["model"] in {"editor-local-loading", "editor-local-idle"}:
            self.hold_local_response(payload["model"] == "editor-local-idle")
            return
        if not cloud and payload["model"] == "editor-local-missing":
            self.send_records([json.dumps({"error": "private"})], status=404)
            return
        if not cloud and payload["model"] == "editor-local-restart" and not self.server.local_restart_observed:
            self.server.local_restart_observed = True
            self.send_records([json.dumps({"error": "private"})], status=503)
            return
        if payload["model"] == "limited":
            self.send_records([json.dumps({"error": "private"})], status=429)
            return
        retry_after = {"limited-with-delay": "30", "limited-with-long-delay": "3600", "limited-with-invalid-delay": "invalid"}.get(
            payload["model"]
        )
        if retry_after is not None:
            self.send_records([json.dumps({"error": "private"})], status=429, retry_after=retry_after)
            return
        if payload["model"] in {"truncated-authentication", "truncated-rate-limit"}:
            status = 401 if payload["model"] == "truncated-authentication" else 429
            data = b'{"error":"private'
            self.send_response(status)
            self.send_header("Content-Length", str(len(data) + 1))
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            self.wfile.write(data)
            return
        if payload["model"] == "malformed":
            self.send_records(["not json\n"], status=200)
            return
        if payload["model"] in {"oversized-record", "oversized-final-fragment"}:
            padding = "x" * (256 * 1024)
            if cloud:
                records = ["data:[DONE]\n", ":" + padding]
            else:
                records = [json.dumps({"done": True, "padding": padding})]
            if payload["model"] == "oversized-record":
                records.append("\n")
            self.send_records(records)
            return
        if payload["model"] == "history":
            messages = payload.get("messages", [])
            if len(messages) != 3 or messages[1].get("tool_calls", [{}])[0].get("function", {}).get("name") != "scene_query":
                self.send_records(["{}"], status=400)
                return
            if messages[2].get("role") != "tool" or (cloud and messages[2].get("tool_call_id") != "call-1"):
                self.send_records(["{}"], status=400)
                return
            if not cloud and messages[2].get("tool_name") != "scene_query":
                self.send_records(["{}"], status=400)
                return
        self.send_model_stream(payload["model"], cloud)

    def send_inline_patch(self, payload):
        messages = payload.get("messages", [])
        if (self.headers.get("Authorization") is not None or len(messages) != 2 or payload.get("tools")
                or messages[-1] != {"role": "user", "content": "Propose one source edit"}
                or messages[0].get("role") != "user"
                or not messages[0].get("content", "").endswith("\n\nint value = 1;\n")):
            self.send_records(["{}"], status=400)
            return
        patch = "```diff\n--- a/code.cpp\n+++ b/code.cpp\n@@ -1 +1 @@\n-int value = 1;\n+int value = 2;\n```"
        frames = [{"message": {"content": patch}}, {"done": True}]
        self.send_records([json.dumps(frame) + "\n" for frame in frames])

    def send_multi_inline_patch(self, payload):
        messages = payload.get("messages", [])
        if (len(messages) != 3 or payload.get("tools") or self.headers.get("Authorization") is not None
                or messages[-1] != {"role": "user", "content": "Propose two source edits"}
                or not messages[0].get("content", "").endswith("\n\ntwo\n")
                or not messages[1].get("content", "").endswith("\n\none\n")):
            self.send_records(["{}"], status=400)
            return
        patch = {"version": 1, "files": [
            {"kind": "edit", "path": "two_very_long_source_name_for_a_narrow_review_card_in_the_local_agent_panel.cpp",
             "hunks": [{"offset": 0, "before": "two", "after": "new two"}]},
            {"kind": "edit", "path": "one.cpp", "hunks": [
                {"offset": 0, "before": "o", "after": "O"},
                {"offset": 2, "before": "e", "after": "E"},
            ]},
        ]}
        frames = [{"message": {"content": json.dumps(patch)}}, {"done": True}]
        self.send_records([json.dumps(frame) + "\n" for frame in frames])

    def send_dock_lifecycle(self, payload):
        self.send_gated_chat(payload, self.server.chat_gate, "Keep this request running", "First visible line",
                             "Final answer after hiding the dock")

    def send_project_lifecycle(self, payload):
        messages = payload.get("messages")
        if messages == [{"role": "user", "content": "Reply for replacement workspace"}]:
            self.send_records(['{"message":{"content":"Replacement workspace answer"}}\n', '{"done":true}\n'])
            return
        gate = self.server.chat_gate / "project"
        gate.mkdir(exist_ok=True)
        self.send_gated_chat(payload, gate, "Reply for retiring workspace", "Retiring workspace started", "Retired workspace answer")

    def send_gated_chat(self, payload, gate, prompt, streamed, completed):
        if (self.headers.get("Authorization") is not None
                or payload.get("messages") != [{"role": "user", "content": prompt}]
                or payload.get("tools") or (gate / "started").exists()):
            self.send_records(["{}"], status=400)
            return
        first = (json.dumps({"message": {"content": streamed + "\n"}}) + "\n").encode("utf-8")
        final = (json.dumps({"message": {"content": completed}}) + '\n{"done":true}\n').encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Content-Length", str(len(first) + len(final)))
        self.end_headers()
        self.wfile.write(first)
        self.wfile.flush()
        (gate / "started").touch()
        if not self.wait_for_release(gate):
            return
        self.wfile.write(final)
        self.wfile.flush()
        (gate / "finished").touch()

    def wait_for_release(self, gate):
        deadline = time.monotonic() + 12
        while not (gate / "release").exists():
            if time.monotonic() >= deadline or self.server.release_pending.wait(timeout=0.02):
                self.close_connection = True
                return False
        return True

    def hold_local_response(self, begin_stream):
        first = b'{"message":{"content":"Pending local draft\\n"}}\n' if begin_stream else b""
        self.hold_response("application/x-ndjson", first)

    def hold_response(self, content_type, first):
        self.close_connection = True
        if first:
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(first)
            self.wfile.flush()
        self.server.release_pending.wait(timeout=10)

    def send_editor_local_tool(self, payload):
        messages = payload.get("messages", [])
        tools = [tool.get("function", {}).get("name") for tool in payload.get("tools", [])]
        if self.headers.get("Authorization") is not None or tools != ["scene.inspect"]:
            self.send_records(["{}"], status=400)
            return
        if payload["model"] == "editor-approved-tool" and not self.server.approved_tool_probe:
            self.send_records(["{}"], status=400)
            return
        if len(messages) == 1:
            if messages[0].get("content") == "Hold the approved scene query":
                gate = self.server.chat_gate / "scene-query"
                gate.mkdir(exist_ok=True)
                (gate / "started").touch()
                if not self.wait_for_release(gate):
                    return
            frames = [{"message": {"tool_calls": [{"function": {"name": "scene.inspect", "arguments": {}}}]}},
                      {"done": True, "eval_count": 2}]
        elif (len(messages) == 3 and messages[1].get("tool_calls", [{}])[0].get("function", {}).get("name") == "scene.inspect"
              and messages[2] == {"role": "tool", "tool_name": "scene.inspect", "content": '{"count":3}'}):
            frames = [{"message": {"content": "Three objects"}}, {"done": True, "eval_count": 2}]
        else:
            self.send_records(["{}"], status=400)
            return
        self.send_records([json.dumps(frame) + "\n" for frame in frames])

    def send_cloud_records(self, model, frames):
        separator = "" if model == "nospace" else " "
        records = [f"data:{separator}{json.dumps(frame)}\r\n\r\n" for frame in frames] + [f"data:{separator}[DONE]\r\n\r\n"]
        self.send_records(records, content_type="text/event-stream")

    def send_model_stream(self, model, cloud):
        if model == "editor-cloud-idle":
            self.hold_response("text/event-stream", b'data: {"choices":[{"delta":{"content":"first"}}]}\r\n\r\n')
            return
        if model == "buffer-allocation":
            text = "x" * 32768
            frames = ([{"choices": [{"delta": {"content": text}}]}] if cloud else
                      [{"message": {"content": text}}, {"done": True}])
        elif model == "tools":
            if cloud:
                frames = [
                    {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "call-1", "function": {"name": "scene_query", "arguments": "{\"target\":"}}]}}]},
                    {"choices": [{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": "\"scene\"}"}}]}}]},
                    {"choices": [], "usage": {"prompt_tokens": 7, "completion_tokens": 2}},
                ]
            else:
                frames = [
                    {"message": {"tool_calls": [{"function": {"name": "scene_query", "arguments": {"target": "scene"}}}]}},
                    {"done": True, "prompt_eval_count": 7, "eval_count": 2},
                ]
        elif cloud:
            frames = [
                {"choices": [{"delta": {"content": "hello"}}]},
                {"choices": [{"delta": {"content": " world"}}]},
                {"choices": [], "usage": {"prompt_tokens": 7, "completion_tokens": 2}},
            ]
        else:
            frames = [
                {"message": {"content": "hello"}},
                {"message": {"content": " world"}},
                {"done": True, "prompt_eval_count": 7, "eval_count": 2},
            ]
        if cloud:
            self.send_cloud_records(model, frames)
        else:
            self.send_records([json.dumps(frame) + "\n" for frame in frames])


def main():
    if len(sys.argv) != 3:
        raise ValueError("CTest must supply its test binary and exact configured test directory")
    binary = Path(sys.argv[1]).resolve(strict=True)
    test_directory = Path(sys.argv[2]).resolve(strict=True)
    specs = {
        "HoroModelProviderTests": [],
        "HoroEditorLocalAgentPanelRenderTests": ["[chat-http]"],
    }
    name = binary.stem if binary.suffix == ".exe" else binary.name
    if binary.parent != test_directory or name not in specs or not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("CTest must supply a configured model or chat test executable")
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.local_restart_observed = False
    server.approved_tool_probe = False
    server.release_pending = threading.Event()
    gate = tempfile.TemporaryDirectory(prefix="horo-chat-gate-")
    server.chat_gate = Path(gate.name)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    environment = os.environ.copy()
    environment["HORO_MODEL_TEST_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    environment["HORO_MODEL_TEST_CHAT_GATE"] = gate.name
    # A proxy request would reach the fixture with an absolute URL and be rejected.
    environment["http_proxy"] = environment["HORO_MODEL_TEST_ENDPOINT"]
    environment["NO_PROXY"] = ""
    environment["no_proxy"] = ""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as offline:
            offline.bind(("127.0.0.1", 0))
            environment["HORO_MODEL_TEST_OFFLINE_ENDPOINT"] = f"http://127.0.0.1:{offline.getsockname()[1]}"
            # The executable is resolved in the exact CMake-owned test directory, including external worktree builds.
            # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit, python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args
            return subprocess.run([str(binary), *specs[name]], env=environment, check=False, timeout=45).returncode  # nosec B603
    finally:
        server.release_pending.set()
        server.shutdown()
        server.server_close()
        worker.join()
        gate.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
