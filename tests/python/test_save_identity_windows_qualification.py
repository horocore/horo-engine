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


def test_release_save_qualification_is_mandatory_and_debug_skips_are_explicit() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    build = workflow.split("      - name: Build Windows Release MCP and restore allocation qualification", 1)[1].split("      - name:", 1)[0]
    assert "--target HoroMcpSessionTests HoroRuntimeSaveRestoreTransactionTests HoroRuntimeSaveSlotLifecycleTests" in build
    step = workflow.split("      - name: Test Windows Release Save lifecycle allocation qualification", 1)[1].split("      - name:", 1)[0]
    assert "runner.os == 'Windows'" in step
    assert "steps.mcp_release_build.outcome == 'success'" in step
    assert "--no-tests=error --timeout 90" in step
    assert "Lifecycle allocation failure after visibility preserves publication knowledge" in step
    assert "Retention allocation failure after selection preserves publication and binding ownership" in step
    assert "--output-junit save-allocation-ctest.xml" in step
    assert "--test-output-size-passed 16384" in step
    assert "--output-junit build/ci-audio-release/" not in step
    verifier = workflow.split("      - name: Verify Windows Release Save allocation variants", 1)[1].split("      - name:", 1)[0]
    assert "steps.save_allocation_release_tests.outcome == 'success'" in verifier
    assert "python scripts/verify_save_allocation_qualification.py build/ci-audio-release/save-allocation-ctest.xml" in verifier
    assert "continue-on-error" not in step + verifier
    assert "github.head_ref == 'fix/HORO-2421_windows_save_allocation_recovery'" not in workflow
    assert workflow.count("github.head_ref == 'fix/HORO-2421_windows_save_allocation_capture'") == 3
    reason = "Unsupported MSVC Debug Error transport under global allocation injection"
    for filename in ("SaveSlotLifecycleAllocationTests.cpp", "SaveSlotRetentionFailureTests.cpp"):
        source = (ROOT / "tests/unit/runtime/save" / filename).read_text(encoding="utf-8")
        assert "#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0" in source
        assert f'SKIP("{reason}; required in Windows Release qualification");' in source
        assert "GENERATE(false, true)" in source


def test_save_junit_path_qualification_uses_real_ctest_from_an_outer_directory() -> None:
    source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    name = "HoroSaveJunitOutputPathTest"
    assert f"add_test(NAME {name}" in source
    assert f'set_tests_properties({name} PROPERTIES LABELS "unit;save;cmake;ci-windows"' in source
    fixture = (ROOT / "tests/cmake/RunSaveJunitOutputPathTest.cmake").read_text(encoding="utf-8")
    assert '"--test-dir" "${build}"' in fixture
    assert '"--output-junit" "save-allocation-ctest.xml"' in fixture
    assert 'WORKING_DIRECTORY "${outer}"' in fixture
    assert 'set(report "${build}/save-allocation-ctest.xml")' in fixture
    assert 'file(READ "${report}" xml)' in fixture


def test_save_junit_capture_qualifies_default_truncation_and_bounded_complete_output() -> None:
    fixture = (ROOT / "tests/cmake/RunSaveJunitOutputPathTest.cmake").read_text(encoding="utf-8")
    assert 'string(REPEAT "bounded-output-padding" 100 padding)' in fixture
    assert 'file(READ "${report}" default_xml)' in fixture
    assert 'default_xml MATCHES "save-junit-path-qualified"' in fixture
    assert 'NOT default_xml MATCHES "1024 bytes"' in fixture
    assert '"--test-output-size-passed" "16384"' in fixture
    assert '"save-junit-path-qualified")' in fixture
