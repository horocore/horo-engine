#!/bin/bash
set -eu
python3 -I - <<'PYTHON'
import json
import os
from pathlib import Path

repository = "horocore/horo-engine"
try:
    event_name = os.environ["GITHUB_EVENT_NAME"]
    if os.environ["GITHUB_REPOSITORY"] != repository:
        raise ValueError("repository")
    if event_name not in {"push", "workflow_dispatch", "pull_request"}:
        raise ValueError("event")
    with Path(os.environ["GITHUB_EVENT_PATH"]).open("rb") as source:
        payload = source.read(2 * 1024 * 1024 + 1)
    if len(payload) > 2 * 1024 * 1024:
        raise ValueError("payload size")
    event = json.loads(payload)
    if event["repository"]["full_name"] != repository:
        raise ValueError("event repository")
    if event_name == "pull_request":
        if event["pull_request"]["head"]["repo"]["full_name"] != repository:
            raise ValueError("fork")
except (KeyError, TypeError, ValueError, OSError):
    print("::error::This Mac accepts only trusted repository push, dispatch and same-repository pull request jobs.")
    raise SystemExit(1)
print("Runner admission: trusted repository event accepted.")
PYTHON
