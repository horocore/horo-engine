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


def test_recovery_branch_captures_exact_windows_binary_stacks_without_weakening_tests() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    names = ("Diagnose Windows Save lifecycle allocation boundary",
             "Diagnose Windows Save retention allocation boundary",
             "Upload Windows Save allocation stack evidence")
    for name in names:
        step = workflow.split(f"      - name: {name}", 1)[1].split("      - name:", 1)[0]
        gate = next(line for line in step.splitlines() if line.strip().startswith("if:"))
        assert "github.event_name == 'pull_request'" in gate
        assert "runner.os == 'Windows'" in gate
        assert "(github.head_ref == 'fix/HORO-2421_windows_save_allocation_capture' || " in gate
        assert "github.head_ref == 'fix/HORO-2421_windows_save_allocation_recovery')" in gate
        assert "continue-on-error" not in step
        if name.startswith("Diagnose"):
            assert "steps.debug_build.outcome == 'success'" in gate
            assert "timeout-minutes: 3" in step
            assert "-CaptureSymbolEvidence" in step
