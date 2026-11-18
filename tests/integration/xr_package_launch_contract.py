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
record = f"horo-xr-package-probe-v1 {install} linux system-default vulkan {target} 9 8 7 1 3 0 2 2 3"
launch = Path(sys.argv[1]).resolve(strict=True)

with tempfile.TemporaryDirectory(prefix="horo installed probe ") as directory:
    root = Path(directory).resolve()
    project = root / "workspace å with space"
    project.mkdir()
    trust = [root / name for name in ("roots", "catalog", "signature")]
    for path in trust:
        path.touch()
    for variant in ("accepted", "foreign", "malformed", "duplicate", "failed", "stale-executable", "surviving-child", "interrupted"):
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
if variant == "foreign":
    record = record.replace({target!r}, {digest(b"different target")!r})
elif variant == "malformed":
    record += " /root/a-sensitive-path"
print(record, flush=True)
if variant == "duplicate":
    print(record, flush=True)
if variant in ("surviving-child", "interrupted"):
    child = subprocess.Popen(['/usr/bin/sleep', '20'])
    Path('pid').write_text(str(child.pid))
    if variant == 'interrupted':
        time.sleep(20)
sys.exit(7 if variant == "failed" else 0)
""", encoding="utf-8")
        executable.chmod(0o700)
        (root / "ran").unlink(missing_ok=True)
        args = [str(launch), "--probe-xr-package", "linux", "system-default", runtime, headset, install, str(executable),
                digest(b"stale binary") if variant == "stale-executable" else digest(executable.read_bytes()),
                str(root), str(project), *(str(path) for path in trust)]
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
        assert (run.returncode == 0) == (variant == "accepted"), (variant, run)
        assert run.stdout == (record + "\n" if variant == "accepted" else ""), (variant, run)
        assert "private" not in run.stderr and (root / "ran").exists() == (variant != "stale-executable"), (variant, run)
        if variant in ("surviving-child", "interrupted"):
            proc_stat = Path("/proc") / (root / "pid").read_text() / "stat"
            for _ in range(20):
                if not proc_stat.exists() or proc_stat.read_text().split()[2] == "Z":
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("owned descendant remained running")
        print(f"{variant}: passed")
