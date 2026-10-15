"""Keep source-edit fault diagnostics bounded and independent of normal CI results."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
SELECTION = (
    "((github.event_name == 'pull_request' && "
    "github.head_ref == 'fix/HORO-224_source_edit_allocation_recovery') || "
    "(github.event_name == 'workflow_dispatch' && inputs.source_allocation_diagnostics == true))"
)


def step(name: str) -> str:
    return WORKFLOW.split(f"      - name: {name}", 1)[1].split("      - name:", 1)[0]


def test_source_stack_dispatch_is_explicit_default_off_and_uses_exact_case() -> None:
    dispatch = WORKFLOW.split("  workflow_dispatch:", 1)[1].split("\npermissions:", 1)[0]
    setting = dispatch.split("      source_allocation_diagnostics:", 1)[1]
    for expected in ("type: boolean", "required: false", "default: false"):
        assert expected in setting
    diagnostic = step("Diagnose Windows source edit allocation boundary")
    assert SELECTION in diagnostic
    assert "!cancelled()" in diagnostic
    assert "runner.os == 'Windows' && steps.debug_build.outcome == 'success'" in diagnostic
    assert "timeout-minutes: 3" in diagnostic
    assert "shell: pwsh" in diagnostic
    assert "./.github/scripts/diagnose_character_input_windows.ps1 -BuildDirectory build/ci" in diagnostic
    assert "-BinaryName HoroSourceFileOpenServiceTests.exe -EvidenceName source-edit-stack" in diagnostic
    assert (
        "-TestCase 'HoroSourceFileOpenServiceTests::Cancelled and allocation-failed source operations "
        "do not publish partial roots' -CaptureSymbolEvidence"
    ) in diagnostic
    assert "continue-on-error" not in diagnostic
    assert "Test Debug" in WORKFLOW


def test_source_stack_artifact_preserves_provenance_and_does_not_upload_dumps() -> None:
    upload = step("Upload Windows source edit allocation stack evidence")
    assert SELECTION in upload
    assert "always()" in upload
    assert "runner.os == 'Windows'" in upload
    assert "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a" in upload
    assert "source-edit-stack-Windows-${{ github.sha }}-${{ github.run_attempt }}" in upload
    paths = upload.split("          path: |", 1)[1].split("          if-no-files-found:", 1)[0]
    assert set(paths.split()) == {
        "build/ci/source-edit-stack/status.txt",
        "build/ci/source-edit-stack/threads.txt",
        "build/ci/source-edit-stack/ctest.xml",
        "build/ci/source-edit-stack/provenance.json",
    }
    assert "retention-days: 3" in upload
    assert WORKFLOW.count("inputs.source_allocation_diagnostics == true") == 2


def test_normal_debug_suite_still_runs_after_diagnostic_failure() -> None:
    normal = step("Test Debug")
    assert "!cancelled() && steps.debug_build.outcome == 'success'" in normal
    assert "run: ctest --preset" in normal
    assert "source_allocation_diagnostics" not in normal
    assert "continue-on-error" not in normal
