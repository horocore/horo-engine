"""Black-box qualification of the versioned source-free package author tool."""

import json
import os
from pathlib import Path
import shutil
import subprocess  # nosec B404 - this black-box test must execute the staged CLI.
import sys
import tempfile
import time
import unittest


class PackageCommands(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="horo-package-cli-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.source = self.root / "source files"
        (self.source / "assets").mkdir(parents=True)
        (self.source / "horo-package.toml").write_text("schemaVersion = 1\n", encoding="utf-8")
        (self.source / "assets" / "payload.txt").write_bytes(b"portable package payload")
        (self.source / "assets" / "café.txt").write_bytes(b"unicode path")

    def run_tool(self, *arguments, tool=None, expected=0):
        # CTest supplies the staged binary; arguments are test-owned and no shell is involved.
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
        result = subprocess.run([str(tool or self.tool), *map(str, arguments)],  # nosec B603
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, expected, (arguments, result.stdout, result.stderr))
        return result

    def pack(self, name="package.horopkg"):
        artifact = self.root / name
        self.run_tool("pack", self.source, artifact)
        return artifact

    def trust(self, public_key, *, revoked=False, allow_unsigned=False, publisher="com.example.author"):
        document = {
            "schemaVersion": 1,
            "allowUnsigned": allow_unsigned,
            "publishers": [{
                "publisherId": publisher,
                "keyId": "release-1",
                "algorithm": "ecdsa-p256-sha256",
                "publicKeyHex": public_key.hex(),
                "expiresAtUnixMilliseconds": 0,
                "revoked": revoked,
            }],
        }
        path = self.root / "trust.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def signing_key(self):
        private = self.root / "private.pem"
        openssl = shutil.which("openssl")
        if openssl is None:
            raise RuntimeError("OpenSSL is required for the package CLI contract test")
        openssl = str(Path(openssl).resolve(strict=True))
        # Fixed OpenSSL command and test-owned output path; no shell.
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
        subprocess.run([openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(private)],  # nosec B603
                       check=True, capture_output=True)
        private.chmod(0o600)
        # Fixed OpenSSL command and test-owned input path; no shell.
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
        public_der = subprocess.run([openssl, "pkey", "-in", str(private), "-pubout", "-outform", "DER"],  # nosec B603
                                    check=True, capture_output=True).stdout
        public = public_der[-65:]
        self.assertEqual(public[0], 4)
        return private, public

    def test_pack_reproducible_and_source_free_distribution(self):
        first = self.pack("first.horopkg")
        os.utime(self.source / "assets" / "payload.txt", (time.time() + 60, time.time() + 60))
        second = self.pack("second.horopkg")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertIn("files=3", self.run_tool("inspect", first).stdout)
        self.assertIn("1.0.0", self.run_tool("--version").stdout)
        copied = self.root / "distributed-sdk"
        shutil.copytree(self.sdk_root, copied)
        consumer = copied / "bin" / self.tool.name
        self.assertIn("files=3", self.run_tool("inspect", first, tool=consumer).stdout)

    def test_rejects_malformed_and_unsafe_inputs(self):
        malformed = self.root / "bad.horopkg"
        malformed.write_bytes(b"not a zip archive")
        self.run_tool("inspect", malformed, expected=1)
        self.run_tool("pack", self.source, self.source / "inside.horopkg", expected=1)
        if os.name != "nt":
            alias = self.root / "source-alias"
            alias.symlink_to(self.source, target_is_directory=True)
            self.run_tool("pack", self.source, alias / "inside-via-alias.horopkg", expected=1)
        self.run_tool("pack", self.source, self.pack(), expected=1)
        if os.name != "nt":
            (self.source / "assets" / "link").symlink_to(self.source / "horo-package.toml")
            self.run_tool("pack", self.source, self.root / "symlink.horopkg", expected=1)

    def test_rejects_missing_manifest_reserved_name_and_bad_invocation(self):
        (self.source / "horo-package.toml").unlink()
        self.run_tool("pack", self.source, self.root / "missing.horopkg", expected=1)
        (self.source / "horo-package.toml").write_text("schemaVersion = 1\n", encoding="utf-8")
        (self.source / "files.manifest.json").write_text("{}", encoding="utf-8")
        self.run_tool("pack", self.source, self.root / "reserved.horopkg", expected=1)
        self.run_tool("sign", expected=2)
        self.run_tool("inspect", "--unknown", expected=1)

    def test_pack_rejects_private_signing_key_content(self):
        private, _ = self.signing_key()
        (self.source / "assets" / "accidental.txt").write_bytes(private.read_bytes())
        output = self.root / "must-not-exist.horopkg"
        result = self.run_tool("pack", self.source, output, expected=1)
        self.assertIn("package.credential_input", result.stderr)
        self.assertNotIn(str(private), result.stderr)
        self.assertFalse(output.exists())

    def test_sign_verify_and_reject_untrusted_or_tampered_evidence(self):
        archive = self.pack()
        private, public = self.signing_key()
        trust = self.trust(public)
        signature = self.root / "signature.json"
        signed = self.run_tool("sign", archive, "--publisher", "com.example.author", "--key-id", "release-1", "--key", private,
                               "--output", signature)
        self.assertNotIn("PRIVATE KEY", signed.stdout + signed.stderr + signature.read_text())
        self.assertIn("verified", self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust,
                                                 "--signature", signature).stdout)
        unknown = self.trust(public, publisher="com.example.unknown")
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", unknown, "--signature", signature,
                      expected=1)
        revoked = self.trust(public, revoked=True)
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", revoked, "--signature", signature,
                      expected=1)
        self.trust(public)
        envelope = json.loads(signature.read_text())
        envelope["signatureHex"] = "00" * 64
        signature.write_text(json.dumps(envelope))
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust, "--signature", signature,
                      expected=1)
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust, expected=1)
        self.trust(public, allow_unsigned=True)
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust)

    def test_rejects_malformed_trust_and_signature_documents(self):
        archive = self.pack()
        private, public = self.signing_key()
        trust = self.trust(public)
        signature = self.root / "signature.json"
        self.run_tool("sign", archive, "--publisher", "com.example.author", "--key-id", "release-1", "--key", private,
                      "--output", signature)
        trust.write_text('{"schemaVersion":1,"schemaVersion":1,"allowUnsigned":false,"publishers":[]}')
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust, "--signature", signature,
                      expected=1)
        self.trust(public)
        signature.write_bytes(b"")
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust, "--signature", signature,
                      expected=1)
        self.trust(public)
        trust.write_bytes(b"")
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust, expected=1)
        self.trust(public)
        signature.write_text('{"schemaVersion":1,"signatureHex":"oops"}')
        self.run_tool("verify", archive, "--package-id", "com.example.package", "--trust", trust, "--signature", signature,
                      expected=1)
        self.run_tool("verify", archive, "--package-id", "not an id", "--trust", trust, expected=1)

    @unittest.skipIf(os.name == "nt", "POSIX permission contract")
    def test_sign_rejects_shared_private_key_permissions(self):
        archive = self.pack()
        private, _ = self.signing_key()
        private.chmod(0o644)
        result = self.run_tool("sign", archive, "--publisher", "com.example.author", "--key-id", "release-1", "--key", private,
                               "--output", self.root / "signature.json", expected=1)
        self.assertIn("package.key_permissions", result.stderr)
        self.assertNotIn("PRIVATE KEY", result.stderr)


if __name__ != "__main__":
    # Repository pytest has no staged SDK arguments; CTest is this suite's integration runner.
    PackageCommands = unittest.skip("requires staged SDK tool; covered by HoroPackageAuthorCommands CTest")(PackageCommands)
else:
    if len(sys.argv) != 3:
        raise RuntimeError("expected staged SDK root and tool path")
    PackageCommands.sdk_root = Path(sys.argv[1])
    PackageCommands.tool = Path(sys.argv[2])
    sys.argv[:] = sys.argv[:1]
    unittest.main()
