"""Windows must build and discover the direct UUID allocation recovery regression."""
from __future__ import annotations

import re

from test_ci_workflow_grouping import ROOT, targets


def test_windows_save_identity_allocation_has_build_and_discovery_closure() -> None:
    target = "HoroRuntimeSaveIdentityTests"
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert {target, "HoroRuntimeSaveSlotLifecycleTests"} <= targets("HORO_CI_WINDOWS_TARGETS")
    declaration = re.search(rf"add_executable\({target}\s+(.*?)\n\)", source, re.S)
    assert declaration
    assert {"support/AllocationProbe.cpp", "unit/runtime/save/SaveIdentityTests.cpp"} <= set(declaration.group(1).split())
    registration = re.search(r"set\(HORO_CATCH_TEST_TARGETS\s+(.*?)\n\)", source, re.S)
    assert registration and target in registration.group(1).split()
