"""Shared offline import of the annotation validator under test."""

import importlib.util
from pathlib import Path
import sys

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "pr_annotations.py"
SPEC = importlib.util.spec_from_file_location("pr_annotations", SCRIPT)
assert SPEC is not None
assert SPEC.loader is not None
annotations = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = annotations
SPEC.loader.exec_module(annotations)
