"""Exercise the production Linux Desktop XR probe launcher with a supervised fixture."""
from pathlib import Path
import hashlib
import os
import signal
import subprocess
import sys
import tempfile
import time

def digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


install = digest(b"fixture install")
runtime = "Selected Runtime"
headset = "Selected HMD"
target = digest(b"Horo Desktop OpenXR probe v1\0" + runtime.encode() + b"\0" + headset.encode())
record = f"horo-xr-package-probe-v1 {install} linux system-default vulkan {target} 9 8 7 1 3 0 2 2 11"
failure = f"horo-xr-package-probe-failure-v1 {install} linux system-default vulkan {target} desktop-local permission-denied"
launch = Path(sys.argv[1]).resolve(strict=True)

with tempfile.TemporaryDirectory(prefix="horo installed probe ") as directory:
    root = Path(directory).resolve()
    project = root / "workspace å with space"
    project.mkdir()
    trust = [root / name for name in ("roots", "catalog", "signature")]
    for path in trust:
        path.touch()
    success_variants = ("accepted", "foreign", "malformed", "duplicate", "failed", "stale-executable", "surviving-child", "interrupted")
    failure_variants = ("failure-receipt", "foreign-failure", "malformed-failure", "failure-exit-mismatch", "mixed-receipts",
                        "stale-after-failure", "wrong-connection", "failure-on-stderr")
    qualified_variants = ("accepted", "failure-receipt", "foreign", "duplicate", "stale-executable", "stale-after-failure",
                          "surviving-child", "interrupted", "timed-out", "unsupported-route-mask", "invalid-connection", "invalid-timeout")
    cases = [(variant, False) for variant in (*success_variants, *failure_variants)]
    cases.extend((variant, True) for variant in qualified_variants)
    for variant, qualified in cases:
        executable = root / "selected editor"
        executable.write_text(f"""#!/usr/bin/python3
import os, subprocess, sys, time
from pathlib import Path
assert all(key not in os.environ for key in ("XR_RUNTIME_JSON", "XR_ENABLE_API_LAYERS", "XR_API_LAYER_PATH", "XR_LOADER_DEBUG"))
assert sys.argv[1:] == {['--renderer', 'vulkan', '--project', str(project), '--xr-host-roots', str(trust[0]), '--xr-host-catalog', str(trust[1]), '--xr-host-catalog-signature', str(trust[2]), '--xr-package-probe', 'linux', 'system-default', runtime, headset, install]!r}
print('private-path /root/secret-setting')
print('private-device-data', file=sys.stderr)
Path('ran').write_text('1')
variant = {variant!r}
record = {record!r}
failure = {failure!r}
if variant == "foreign":
    record = record.replace({target!r}, {digest(b"different target")!r})
elif variant == "malformed":
    record += " /root/a-sensitive-path"
elif variant == "unsupported-route-mask":
    record = record.rsplit(" ", 1)[0] + " 16"
if variant == 'foreign-failure':
    failure = failure.replace({target!r}, {digest(b"different target")!r})
if variant == 'malformed-failure':
    failure = failure.replace('permission-denied', '/root/a-sensitive-path')
if variant == 'wrong-connection':
    failure = failure.replace('desktop-local', 'remote-device')
if variant in {failure_variants!r}:
    print(failure, file=sys.stderr if variant == 'failure-on-stderr' else sys.stdout, flush=True)
    if variant == 'mixed-receipts':
        print(record, flush=True)
    if variant == 'stale-after-failure':
        Path(sys.argv[0]).write_text('changed executable')
    sys.exit(0 if variant == 'failure-exit-mismatch' else 1)
print(record, flush=True)
if variant == "duplicate":
    print(record, flush=True)
if variant in ("surviving-child", "interrupted", "timed-out"):
    child = subprocess.Popen(['/usr/bin/sleep', '20'])
    Path('pid').write_text(str(child.pid))
    if variant in ('interrupted', 'timed-out'):
        time.sleep(20)
sys.exit(7 if variant == "failed" else 0)
""", encoding="utf-8")
        executable.chmod(0o700)
        (root / "ran").unlink(missing_ok=True)
        executable_digest = digest(b"stale binary") if variant == "stale-executable" else digest(executable.read_bytes())
        args = [str(launch), "--probe-xr-package", "linux", "system-default", runtime, headset, install, str(executable),
                executable_digest, str(root), str(project), *(str(path) for path in trust)]
        if qualified:
            connection = "remote-device" if variant == "invalid-connection" else "desktop-local"
            args += ["--connection", connection]
            if variant == "timed-out":
                args += ["--timeout-seconds", "1"]
            if variant == "invalid-timeout":
                args += ["--timeout-seconds", "91"]
        env = {**os.environ, **{key: "do not forward" for key in ("XR_RUNTIME_JSON", "XR_ENABLE_API_LAYERS", "XR_API_LAYER_PATH", "XR_LOADER_DEBUG")}}
        (root / "pid").unlink(missing_ok=True)
        if variant == "interrupted":
            process = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            for _ in range(100):
                if (root / "pid").exists():
                    break
                time.sleep(0.03)
            else:
                process.kill()
                raise AssertionError("probe fixture did not initialize")
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=8)
            run = subprocess.CompletedProcess(args, process.returncode, stdout, stderr)
        else:
            run = subprocess.run(args, env=env, timeout=8, capture_output=True, text=True, check=False)
        invalid_options = variant in ("invalid-connection", "invalid-timeout")
        assert run.returncode == (0 if variant == "accepted" else 2 if invalid_options else 1), (variant, run)
        assert "private" not in run.stderr and "/root/" not in run.stderr, (variant, run)
        assert (root / "ran").exists() == (not invalid_options and variant != "stale-executable"), (variant, run)
        if qualified and not invalid_options:
            identity = f"horo-xr-desktop-qualification-v1 {install} {executable_digest} linux system-default vulkan {target} desktop-local"
            if variant == "accepted":
                assert run.stdout == identity + " preflight 9 8 7 1 3 0 2 2 11\n" and not run.stderr, (variant, run)
            else:
                outcomes = {"failure-receipt": "permission-denied", "foreign": "unattributed-receipt", "duplicate": "unattributed-receipt",
                            "stale-executable": "stale-or-busy-package", "stale-after-failure": "stale-package", "timed-out": "timed-out",
                            "surviving-child": "interrupted-or-retired", "interrupted": "interrupted-or-retired",
                            "unsupported-route-mask": "unattributed-receipt"}
                assert run.stderr == f"{identity} {outcomes[variant]}\n" and not run.stdout, (variant, run)
            assert runtime not in run.stderr + run.stdout and headset not in run.stderr + run.stdout
        else:
            assert run.stdout == (record + "\n" if variant == "accepted" else ""), (variant, run)
            if variant == "failure-receipt":
                assert run.stderr == failure + "\n", (variant, run)
            else:
                assert "horo-xr-package-probe-failure-v1" not in run.stderr, (variant, run)
        if variant in ("surviving-child", "interrupted", "timed-out"):
            proc_stat = Path("/proc") / (root / "pid").read_text() / "stat"
            for _ in range(20):
                if not proc_stat.exists() or proc_stat.read_text().split()[2] == "Z":
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("owned descendant remained running")
        print(f"{variant} ({'explicit' if qualified else 'legacy'}): passed")
