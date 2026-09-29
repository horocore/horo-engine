"""Exercise the update adapter against an ephemeral localhost TLS server."""

import os
from pathlib import Path
import shutil
import ssl
import subprocess  # nosec B404 - Test only: commands use argument arrays without a shell.
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PAYLOAD = b"verified-update-package"


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get("Range"), self.headers.get("If-Range")))
        mode = self.server.mode
        if mode == "redirect":
            self.send_response(302)
            self.send_header("Location", f"https://127.0.0.1:{self.server.server_port}/redirected")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if mode == "resume":
            body = PAYLOAD[7:]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes 7-{len(PAYLOAD) - 1}/{len(PAYLOAD)}")
        else:
            body = PAYLOAD
            self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", '"version-1"')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def generate_certificate(root: Path, openssl: str) -> tuple[Path, Path]:
    certificate = root / "localhost.crt"
    private_key = root / "localhost.key"
    configuration = root / "certificate.cnf"
    configuration.write_text("[req]\ndistinguished_name=dn\nx509_extensions=v3_req\nprompt=no\n"
                             "[dn]\nCN=127.0.0.1\n[v3_req]\nsubjectAltName=IP:127.0.0.1\n",
                             encoding="ascii")
    # Resolved openssl path and generated temporary paths; shell remains disabled.
    # nosemgrep
    subprocess.run(  # nosec B603
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-config", str(configuration),
         "-keyout", str(private_key), "-out", str(certificate)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return certificate, private_key


def run_scenarios(server: ThreadingHTTPServer, root: Path, certificate: Path, client: str) -> None:
    url = f"https://127.0.0.1:{server.server_port}/package.zip"
    environment = os.environ.copy()
    environment["NO_PROXY"] = "127.0.0.1"
    environment.pop("HTTPS_PROXY", None)
    environment.pop("https_proxy", None)
    for mode in ("fresh", "resume", "bad-range", "redirect", "untrusted"):
        server.mode = mode
        server.requests.clear()
        stage = root / mode
        stage.mkdir()
        ca_bundle = "" if mode == "untrusted" else str(certificate)
        # CMake supplies the built test executable; fixed arguments and no shell.
        # nosemgrep
        subprocess.run([client, url, ca_bundle, str(stage), mode],  # nosec B603
                       check=True, env=environment, timeout=20)
        if mode == "untrusted":
            if server.requests:
                raise AssertionError("untrusted TLS peer received an HTTP request")
            continue
        if len(server.requests) != 1 or server.requests[0][0] != "/package.zip":
            raise AssertionError(f"unexpected request sequence for {mode}: {server.requests}")
        _, byte_range, if_range = server.requests[0]
        expected_range = "bytes=7-" if mode in ("resume", "bad-range") else None
        expected_validator = '"version-1"' if expected_range else None
        if byte_range != expected_range or if_range != expected_validator:
            raise AssertionError(f"unexpected range headers for {mode}: {server.requests}")


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    openssl = shutil.which("openssl")
    if not openssl:
        print("openssl unavailable; local HTTPS integration requires certificate generation", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="horo-update-https-") as temporary:
        root = Path(temporary)
        certificate, private_key = generate_certificate(root, openssl)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests = []
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, private_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            run_scenarios(server, root, certificate, sys.argv[1])
        finally:
            server.shutdown()
            thread.join(timeout=5)
            server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
