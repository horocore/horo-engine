"""Exercise both production HTTP adapters against deterministic loopback wire protocols."""

import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def send_records(self, records, content_type="application/json", status=200):
        data = "".join(records).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/api/tags":
            self.send_records([json.dumps({"models": [{"name": "normal"}]})])
        elif self.path == "/v1/models" and self.headers.get("Authorization") == "Bearer fixture-token":
            self.send_records([json.dumps({"data": [{"id": "normal"}]})])
        else:
            self.send_records([json.dumps({"error": "private"})], status=401)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(length))
        cloud = self.path == "/v1/chat/completions"
        if not cloud and self.path != "/api/chat":
            self.send_records(["{}"], status=404)
            return
        if cloud and self.headers.get("Authorization") != "Bearer fixture-token":
            self.send_records([json.dumps({"error": "private"})], status=401)
            return
        if payload["model"] == "limited":
            self.send_records([json.dumps({"error": "private"})], status=429)
            return
        if payload["model"] == "malformed":
            self.send_records(["not json\n"], status=200)
            return
        if payload["model"] == "history":
            messages = payload.get("messages", [])
            if len(messages) != 3 or messages[1].get("tool_calls", [{}])[0].get("function", {}).get("name") != "scene_query":
                self.send_records(["{}"], status=400)
                return
            if messages[2].get("role") != "tool" or (cloud and messages[2].get("tool_call_id") != "call-1"):
                self.send_records(["{}"], status=400)
                return
        if payload["model"] == "tools":
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
            records = [f"data: {json.dumps(frame)}\n\n" for frame in frames] + ["data: [DONE]\n\n"]
            self.send_records(records, content_type="text/event-stream")
        else:
            self.send_records([json.dumps(frame) + "\n" for frame in frames])


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    environment = os.environ.copy()
    environment["HORO_MODEL_TEST_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    try:
        return subprocess.call([sys.argv[1]], env=environment)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    raise SystemExit(main())
