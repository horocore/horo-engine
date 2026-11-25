"""Run the native Horo PipeWire contract against two privately owned virtual sessions."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def wait_for(condition, message):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.025)
    raise RuntimeError(message)


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def read_graph(env, dump):
    result = subprocess.run([dump], env=env, capture_output=True, text=True, timeout=2, check=False)
    return [] if result.returncode else json.loads(result.stdout)


def has_default(env, dump, key, expected):
    for node in read_graph(env, dump):
        for entry in node.get("metadata", []):
            if entry.get("key") == key and entry.get("value") == {"name": expected}:
                return True
    return False


def add_default_device(env, logs, tools, label, priority, capture=False):
    _, _, client, dump, _ = tools
    media = "Audio/Source/Virtual" if capture else "Audio/Sink"
    properties = (f"{{ factory.name=support.null-audio-sink media.class={media} object.linger=true "
                  f"node.name={label} node.description={label} priority.session={priority} audio.position=[ FL FR ] }}")
    subprocess.run([client, "create-node", "adapter", properties], env=env, stdout=logs, stderr=logs, check=True, timeout=3)
    key = "default.audio.source" if capture else "default.audio.sink"
    wait_for(lambda: has_default(env, dump, key, label), f"private session manager did not select {label}")


def remove_device(env, logs, tools, label):
    _, _, client, dump, _ = tools
    devices = [node["id"] for node in read_graph(env, dump)
               if node.get("info", {}).get("props", {}).get("node.name") == label]
    if len(devices) != 1:
        raise RuntimeError(f"expected one private PipeWire node named {label}")
    node_id = devices[0]
    if not isinstance(node_id, int) or isinstance(node_id, bool) or not 0 < node_id <= 0xFFFFFFFF:
        raise RuntimeError("invalid private PipeWire node identity")
    subprocess.run([client, "destroy", str(node_id)], env=env, stdout=logs, stderr=logs, check=True, timeout=3)

    def is_removed():
        graph = read_graph(env, dump)
        return bool(graph) and all(node["id"] != node_id for node in graph)

    wait_for(is_removed, f"private PipeWire node {label} was not removed")


def start_session(runtime, env, logs, tools, label, processes):
    daemon, session, _, _, _ = tools
    server = subprocess.Popen([daemon], env=env, stdout=logs, stderr=logs)
    processes.append(server)
    wait_for(lambda: (runtime / "pipewire-0").is_socket(), "private PipeWire server did not start")
    manager = subprocess.Popen([session], env=env, stdout=logs, stderr=logs)
    processes.append(manager)
    add_default_device(env, logs, tools, label, 100)
    return server, manager


def main():
    executable, *tools = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="horo-pipewire-") as directory:
        root = Path(directory)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env = {key: value for key, value in os.environ.items() if not key.startswith(("PIPEWIRE_", "WIREPLUMBER_"))}
        env.update({"XDG_RUNTIME_DIR": str(runtime), "XDG_CONFIG_HOME": str(root / "config"),
                    "XDG_STATE_HOME": str(root / "state"), "XDG_CACHE_HOME": str(root / "cache"),
                    "PIPEWIRE_RUNTIME_DIR": str(runtime), "PIPEWIRE_REMOTE": "pipewire-0",
                    "HORO_PIPEWIRE_TEST_CONTROL": str(root)})
        processes = []
        with (root / "native.log").open("w+") as logs:
            try:
                server, manager = start_session(runtime, env, logs, tools, "horo-test-a", processes)
                add_default_device(env, logs, tools, "horo-test-mic-a", 2000, capture=True)
                backend = subprocess.Popen([executable, "--reporter", "compact"], env=env)
                processes.append(backend)
                wait_for((root / "capture-ready").exists, "initial input was not discovered")
                # CMake resolves the executable. The arguments and private runtime are fixture-owned.
                subprocess.run([tools[-1], "-d", "0"],  # nosec B603
                               env=env, stdout=logs, stderr=logs, check=True, timeout=3)
                (root / "capture-default-cleared").touch()
                wait_for((root / "capture-default-unavailable").exists, "cleared metadata retained the recording default")
                remove_device(env, logs, tools, "horo-test-mic-a")
                add_default_device(env, logs, tools, "horo-test-mic-a", 2000, capture=True)
                wait_for(lambda: has_default(env, tools[3], "default.audio.sink", "horo-test-a"),
                         "private session manager did not restore the playback default")
                (root / "capture-changed").touch()
                wait_for(lambda: (root / "switch-ready").exists(), "initial output did not reach native rendering")
                add_default_device(env, logs, tools, "horo-test-b", 2000)
                (root / "default-changed").touch()
                wait_for(lambda: (root / "started").exists(), "first stream did not reach native rendering")
                stop(server)
                stop(manager)
                wait_for(lambda: (root / "detached").exists(), "lost stream did not prove callback detachment")
                wait_for(lambda: not (runtime / "pipewire-0").exists(), "old PipeWire socket was not retired")
                start_session(runtime, env, logs, tools, "horo-test-c", processes)
                (root / "restarted").touch()
                if backend.wait(timeout=10):
                    raise RuntimeError("native PipeWire backend contract failed after restart")
            except (RuntimeError, subprocess.SubprocessError):
                logs.seek(0)
                print(logs.read(16 * 1024), file=sys.stderr)
                raise
            finally:
                for process in reversed(processes):
                    stop(process)


if __name__ == "__main__":
    main()
