"""Shared offline import of the annotation validator under test."""

import importlib.util
from pathlib import Path
import sys

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "pr_annotations.py"
SPEC = importlib.util.spec_from_file_location("pr_annotations", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise ImportError("cannot load the annotation validator for regression tests")
annotations = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = annotations
SPEC.loader.exec_module(annotations)
