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
    assert "$commands = '~*kn 64;qd'" in SCRIPT
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


def test_pcg_diagnostic_reuses_bounded_reader_for_original_single_point_case() -> None:
    diagnostic = WORKFLOW.split('      - name: Diagnose Windows PCG evaluator completion timeout', 1)[1].split('      - name:', 1)[0]
    assert "runner.os == 'Windows' && steps.debug_build.outcome == 'success'" in diagnostic
    assert "github.event_name == 'pull_request' && github.event.pull_request.number == 3403" in diagnostic
    assert 'timeout-minutes: 3' in diagnostic
    assert '-BinaryName HoroPCGTests.exe -EvidenceName pcg-evaluator-diagnostics' in diagnostic
    assert "-TestCase 'HoroPCGTests::PCG catalog work cost is checked before executable allocation admission'" in diagnostic
    assert 'continue-on-error' not in diagnostic
    assert WORKFLOW.index('Build Debug') < WORKFLOW.index('Diagnose Windows PCG evaluator completion timeout')
    assert WORKFLOW.index('Diagnose Windows PCG evaluator completion timeout') < WORKFLOW.index('      - name: Test Debug')
    upload = WORKFLOW.split('      - name: Upload Windows PCG evaluator stack text', 1)[1].split('      - name:', 1)[0]
    assert 'github.event.pull_request.number == 3403' in upload
    assert 'retention-days: 3' in upload
    assert '*' not in upload
    assert '.dmp' not in upload
    for artifact in ('status.txt', 'threads.txt', 'ctest.xml'):
        assert f'build/ci/pcg-evaluator-diagnostics/{artifact}' in upload


def test_reader_defaults_preserve_character_case_and_reject_path_traversal() -> None:
    assert "[ValidatePattern('^Horo[A-Za-z0-9]+Tests\\.exe$')]" in SCRIPT
    assert "[ValidatePattern('^Horo[A-Za-z0-9]+Tests::[^\\r\\n]+$')]" in SCRIPT
    assert "[ValidatePattern('^[a-z][a-z0-9-]*$')]" in SCRIPT
    assert "$BinaryName = 'HoroCharacterInputTests.exe'" in SCRIPT
    assert "$EvidenceName = 'character-input-diagnostics'" in SCRIPT
    assert "$TestCase = 'HoroCharacterInputTests::Load-time allocation failure preserves caller grant and successful motion path does not allocate'" in SCRIPT
    assert '$case = $TestCase' in SCRIPT


def test_save_capture_keeps_both_exact_cases_and_local_symbol_identity() -> None:
    assert '[switch]$CaptureSymbolEvidence' in SCRIPT
    assert "[System.IO.Path]::ChangeExtension($binary, '.pdb')" in SCRIPT
    assert 'Get-FileHash -LiteralPath $_ -Algorithm SHA256' in SCRIPT
    assert "creationDate = $birth.ToUniversalTime().ToString('o')" in SCRIPT
    assert 'parentPid = $child.ParentProcessId' in SCRIPT
    assert '.symopt+0x400;.reload /f $module.exe;!lmi $module;lmv m $module;~*kn 64;qd' in SCRIPT
    assert '-netsyms:no' in SCRIPT
    assert "symbolMatch = 'UNVERIFIED:" in SCRIPT
    assert "$manifest['ctestExit'] = $result" in SCRIPT
    cases = {
        'lifecycle': 'Lifecycle allocation failure after visibility preserves publication knowledge',
        'retention': 'Retention allocation failure after selection preserves publication and binding ownership',
    }
    for kind, case in cases.items():
        step = WORKFLOW.split(f'      - name: Diagnose Windows Save {kind} allocation boundary', 1)[1].split('      - name:', 1)[0]
        assert "github.head_ref == 'fix/HORO-2421_windows_save_allocation_capture'" in step
        assert "runner.os == 'Windows' && steps.debug_build.outcome == 'success'" in step
        assert '!cancelled()' in step
        assert f"-TestCase 'HoroRuntimeSaveSlotLifecycleTests::{case}' -CaptureSymbolEvidence" in step
        assert '-BinaryName HoroRuntimeSaveSlotLifecycleTests.exe' in step
        assert 'timeout-minutes: 3' in step
        assert 'continue-on-error' not in step
    upload = WORKFLOW.split('      - name: Upload Windows Save allocation stack evidence', 1)[1].split('      - name:', 1)[0]
    assert '*' not in upload
    for kind in cases:
        for artifact in ('status.txt', 'threads.txt', 'ctest.xml', 'provenance.json'):
            assert f'build/ci/save-{kind}-stack/{artifact}' in upload
