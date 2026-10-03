from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ci.yml"
SYMBOL_SCRIPT = ROOT / ".github/scripts/stage_windows_symbols.ps1"


def audio_job() -> str:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    return workflow.split("  test:", 1)[1]


def symbol_step() -> str:
    return audio_job().split("      - name: Prepare Windows mixer allocation diagnostics", 1)[1].split(
        "      - name: Test Debug", 1
    )[0]


def test_symbol_deployment_is_windows_debug_only_and_precedes_the_gate() -> None:
    job = audio_job()
    step = symbol_step()
    assert "runner.os == 'Windows' && steps.debug_build.outcome == 'success'" in step
    assert "shell: pwsh" in step
    assert job.index("Build Debug") < job.index("Prepare Windows mixer allocation diagnostics")
    assert job.index("Prepare Windows mixer allocation diagnostics") < job.index("Test Debug")
    assert "continue-on-error" not in step


def test_symbol_deployment_checks_worker_and_pdb_and_keeps_sdk_dlls_adjacent() -> None:
    step = SYMBOL_SCRIPT.read_text(encoding="utf-8")
    assert "Debuggers\\x64\\dbghelp.dll" in step
    assert r"bin\*\x64\dbghelp.dll" in step
    assert "Sort-Object { $_.VersionInfo.FileVersionRaw } -Descending" in step
    assert "-BinaryDirectory build/ci/tests" in symbol_step()
    assert "Resolve-Path -LiteralPath $BinaryDirectory" in step
    assert "'HoroMixerAllocationFailureWorker.exe', 'HoroMixerAllocationFailureWorker.pdb'" in step
    assert "throw 'Windows SDK x64 symbol reader is unavailable'" in step
    assert 'throw "Allocation diagnostic artifact is missing or empty: $path"' in step
    assert "'dbghelp.dll', 'dbgcore.dll', 'symsrv.dll', 'srcsrv.dll'" in step
    assert "Join-Path $reader.DirectoryName $library" in step
    assert "Copy-Item -LiteralPath $source -Destination (Join-Path $destination $library) -Force" in step


def test_symbol_deployment_does_not_relax_callback_safety() -> None:
    gate = audio_job().split("      - name: Test Debug", 1)[1].split("      - name:", 1)[0]
    assert 'ctest --preset "${{ matrix.preset }}"' in gate
    suites = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
    assert "HoroAudio" in suites
    assert "MixerTests" in suites
    presets = (ROOT / "CMakePresets.json").read_text(encoding="utf-8")
    assert '"noTestsAction": "error"' in presets
    assert "continue-on-error" not in gate
    assert "|| true" not in gate
