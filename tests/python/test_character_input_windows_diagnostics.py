"""Guard the temporary owner-scoped hang evidence without weakening the test gate."""
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (ROOT / '.github/scripts/diagnose_character_input_windows.ps1').read_text(encoding='utf-8')
WORKFLOW = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')


def test_original_windows_timeout_suite_and_assertions_are_preserved() -> None:
    presets = json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))
    windows = next(item for item in presets['testPresets'] if item['name'] == 'ci-windows-debug')
    assert windows['execution'] == {'jobs': 1, 'timeout': 90}
    assert "'--timeout', '90', '--no-tests=error'" in SCRIPT
    assert '[regex]::Escape($case)' in SCRIPT
    assert 'exit $result' in SCRIPT
    assert '$result = $ctest.ExitCode' in SCRIPT
    gate = WORKFLOW.split('      - name: Test Debug', 1)[1].split('      - name:', 1)[0]
    assert 'ctest --preset "${{ matrix.preset }}"' in gate
    assert '!cancelled()' in gate
    assert 'continue-on-error' not in gate


def test_stack_reader_has_exact_owner_identity_and_bounded_noninvasive_scope() -> None:
    assert 'ParentProcessId = $($ctest.Id)' in SCRIPT
    assert '$_.ExecutablePath -eq $binary' in SCRIPT
    assert '$child.CreationDate -eq $birth' in SCRIPT
    assert '[Math]::Abs(($child.CreationDate - $observed.StartTime).TotalMilliseconds) -le 1' in SCRIPT
    assert '$child.ProcessId -eq $observed.Id' in SCRIPT
    assert "@('-pvr', '-p', \"$($observed.Id)\"" in SCRIPT
    assert "'~*kn 64;qd'" in SCRIPT
    assert '$capture.WaitForExit(10000)' in SCRIPT
    assert '$capture.WaitForExit(1000)' in SCRIPT
    assert '$capture.Kill()' in SCRIPT
    assert '$observed.Kill' not in SCRIPT
    assert '$ctest.Kill' not in SCRIPT
    assert '$attempted = $true' in SCRIPT


def test_no_external_tool_fetch_dump_or_environment_capture() -> None:
    assert 'Get-AuthenticodeSignature' in SCRIPT
    assert "O=Microsoft Corporation" in SCRIPT
    assert 'Get-FileHash' in SCRIPT
    assert "Debuggers\\x64\\cdb.exe" in SCRIPT
    for forbidden in ('Invoke-WebRequest', 'DownloadFile', 'procdump', '.dump', 'Get-ChildItem Env:', '-pn'):
        assert forbidden not in SCRIPT
    upload = WORKFLOW.split('      - name: Upload Windows Character input stack text', 1)[1].split('      - name:', 1)[0]
    assert 'retention-days: 3' in upload
    assert 'github.event.pull_request.number == 3362' in upload
    assert '*' not in upload
    assert '.dmp' not in upload
    assert 'status.txt' in upload
    assert 'threads.txt' in upload
    assert 'ctest.xml' in upload


def test_diagnostic_runs_only_after_owned_windows_build_without_hiding_failure() -> None:
    diagnostic = WORKFLOW.split('      - name: Diagnose Windows Character input allocation timeout', 1)[1].split('      - name:', 1)[0]
    assert "runner.os == 'Windows' && steps.debug_build.outcome == 'success'" in diagnostic
    assert "github.event_name == 'pull_request' && github.event.pull_request.number == 3362" in diagnostic
    assert 'timeout-minutes: 3' in diagnostic
    assert 'shell: pwsh' in diagnostic
    assert 'continue-on-error' not in diagnostic
    assert WORKFLOW.index('Build Debug') < WORKFLOW.index('Diagnose Windows Character input allocation timeout')
    assert WORKFLOW.index('Diagnose Windows Character input allocation timeout') < WORKFLOW.index('      - name: Test Debug')
