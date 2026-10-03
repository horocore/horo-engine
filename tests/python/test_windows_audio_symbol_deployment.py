from pathlib import Path


WORKFLOW = Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml"


def audio_job() -> str:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    return workflow.split("  audio-realtime-safety:", 1)[1].split("  cli-process-windows:", 1)[0]


def symbol_step() -> str:
    return audio_job().split("      - name: Stage Windows allocation diagnostic symbols", 1)[1].split(
        "      - name: Gate callback safety", 1
    )[0]


def test_symbol_deployment_is_windows_debug_only_and_precedes_the_gate() -> None:
    job = audio_job()
    step = symbol_step()
    assert "if: runner.os == 'Windows' && matrix.mode == 'Debug'" in step
    assert "shell: pwsh" in step
    assert job.index("Build callback safety targets") < job.index("Stage Windows allocation diagnostic symbols")
    assert job.index("Stage Windows allocation diagnostic symbols") < job.index("Gate callback safety")
    assert "continue-on-error" not in step


def test_symbol_deployment_checks_worker_and_pdb_and_keeps_sdk_dlls_adjacent() -> None:
    step = symbol_step()
    assert "Debuggers\\x64\\dbghelp.dll" in step
    assert r"bin\*\x64\dbghelp.dll" in step
    assert "Sort-Object { $_.VersionInfo.FileVersionRaw } -Descending" in step
    assert "build/audio-realtime-safety/tests" in step
    assert "'HoroMixerAllocationFailureWorker.exe', 'HoroMixerAllocationFailureWorker.pdb'" in step
    assert "throw 'Windows SDK x64 symbol reader is unavailable'" in step
    assert 'throw "Allocation diagnostic artifact is missing or empty: $path"' in step
    assert "'dbghelp.dll', 'dbgcore.dll', 'symsrv.dll', 'srcsrv.dll'" in step
    assert "Join-Path $reader.DirectoryName $library" in step
    assert "Copy-Item -LiteralPath $source -Destination (Join-Path $destination $library) -Force" in step


def test_symbol_deployment_does_not_relax_callback_safety() -> None:
    gate = audio_job().split("      - name: Gate callback safety", 1)[1].split("      - name:", 1)[0]
    assert "HoroAudio" in gate and "MixerTests::" in gate
    assert "--timeout 90 -j 1" in gate
    assert "continue-on-error" not in gate
    assert "|| true" not in gate
