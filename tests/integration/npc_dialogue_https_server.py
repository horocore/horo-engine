"""Qualify the NPC dialogue I/O path against a local TLS provider."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import ssl
# CTest supplies the executable; the test never invokes a shell.
import subprocess  # nosec B404
import sys
import tempfile
import threading
import time

from update_https_server import generate_certificate

MEDIA_TYPE = "application/vnd.horo.npc-dialogue.v1"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.server.requests.append((self.path, self.headers.get("Content-Type"), body))
        mode = self.server.mode
        status, reply = {
            "approved": (200, b"allow\n5\n"),
            "denied": (200, b"deny\n"),
            "unknown": (200, b"allow\n6\n"),
            "text": (200, b"allow\n5\nextra text"),
            "oversize": (200, b"x" * 256),
            "redirect": (307, b""),
            "timeout": (200, b"allow\n5\n"),
        }[mode]
        if mode == "timeout":
            time.sleep(1)
        try:
            self.send_response(status)
            if mode == "redirect":
                self.send_header("Location", "/other")
            self.send_header("Content-Type", MEDIA_TYPE)
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)
        except (ConnectionError, ssl.SSLEOFError):
            if mode not in ("timeout", "oversize"):
                raise

    def log_message(self, *_args):
        pass


def run_client(client: Path, endpoint: str, ca_bundle: str, mode: str) -> None:
    # CTest's built client and fixed arguments from the localhost fixture.
    subprocess.run([str(client), endpoint, ca_bundle, mode], check=True, timeout=8, shell=False)


def qualify(server: ThreadingHTTPServer, client: Path, certificate: Path) -> None:
    endpoint = f"https://127.0.0.1:{server.server_port}/dialogue"
    offline = ("untrusted", "offline", "retired", "queued-retire", "disabled-agent", "stale-grant", "other-route")
    modes = ("approved", "denied", "unknown", "text", "oversize", "redirect", "timeout", *offline)
    for mode in modes:
        server.mode = mode
        server.requests.clear()
        ca_bundle = "" if mode == "untrusted" else str(certificate)
        run_client(client, endpoint, ca_bundle, mode)
        expected = [] if mode in offline else [("/dialogue", MEDIA_TYPE, b"2,20\n")]
        if server.requests != expected:
            raise AssertionError(f"wrong request sequence for {mode}")
    for invalid in (endpoint.replace("https:", "http:"), endpoint.replace("127.0.0.1", "user@127.0.0.1"), endpoint + "?x=1"):
        run_client(client, invalid, str(certificate), "invalid")


def main() -> None:
    openssl = shutil.which("openssl")
    if not openssl:
        raise RuntimeError("openssl unavailable")
    with tempfile.TemporaryDirectory(prefix="horo-npc-dialogue-https-") as temporary:
        certificate, private_key = generate_certificate(Path(temporary), openssl)
        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            server.requests = []
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certificate, private_key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                qualify(server, Path(sys.argv[1]).resolve(strict=True), certificate)
            finally:
                server.shutdown()
                thread.join(timeout=5)


if __name__ == "__main__":
    main()
