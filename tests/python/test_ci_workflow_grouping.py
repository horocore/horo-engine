"""Contracts for runner grouping and closed-PR cancellation selection."""
from __future__ import annotations

from pathlib import Path
import re
import shlex

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")


def job(name: str) -> str:
    match = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  [\w-]+:|\Z)", WORKFLOW, re.M | re.S)
    assert match
    return match.group(1)


def targets(block: str) -> set[str]:
    result = set()
    for line in block.replace("\\\n", " ").splitlines():
        if "cmake --build" in line and "--target" in line:
            args = shlex.split(line)
            for value in args[args.index("--target") + 1:]:
                if value.startswith("--"):
                    break
                result.add(value)
    return result


def test_windows_group_preserves_every_previously_built_target() -> None:
    assert targets(job("windows-contracts")) == {
        "HoroCliCommandRegistryTests", "HoroPlatformTests", "HoroUpdateZipPackageProducerTests",
        "HoroVfxApiTests", "HoroCinematicModelTests", "HoroCinematicRuntimeTests",
        "HoroCinematicPropertyIntegrationTests", "HoroCinematicModelPublicHeaderConsumer",
        "HoroCinematicRuntimePublicHeaderConsumer", "HoroEditorServicesPublicHeaderConsumer",
        "HoroPrefabTests", "HoroPrefabSceneExpansionTests", "HoroPrefabSceneExpansionContractConsumer",
        "HoroAssetRegistryTests",
    }
    assert not (ROOT / ".github/workflows/prefab-foundation-windows.yml").exists()
    assert WORKFLOW.count("name: Format & Tooling Check") == 1


def test_windows_suites_run_independently_and_remain_blocking() -> None:
    block = job("windows-contracts")
    for suite in ("cli", "vfx", "cinematic", "prefab"):
        assert f"id: {suite}_tests" in block
        assert f"steps.{suite}_tests.outcome" in block
        assert f"test-results/{suite}.xml" in block
    assert block.count("continue-on-error: true") == 6
    assert "if: ${{ !cancelled() }}" in block
    assert 'if [[ "$outcome" != success ]]' in block
    assert "exit 1" in block
    assert "^Horo(CliCommandRegistry|Platform|UpdateZipPackageProducer)Tests::" in block
    assert "^HoroVfxApiTests::" in block
    assert "^HoroCinematic(Model|Runtime|PropertyIntegration)Tests::" in block
    assert "^(HoroPrefabTests|HoroPrefabSceneExpansionTests|HoroAssetRegistryTests)::|^HoroPrefabSceneExpansionContractConsumer$" in block


def test_audio_keeps_both_modes_and_all_platforms() -> None:
    block = job("audio-realtime-safety")
    assert block.count("- { platform:") == 3
    for platform in ("Linux GCC", "macOS Clang", "Windows MSVC"):
        assert f"platform: {platform}" in block
    for mode in ("debug", "release"):
        assert f"build/audio-realtime-safety-{mode}" in block
        assert f"steps.audio_build_{mode}.outcome" in block
        assert f"steps.audio_tests_{mode}.outcome" in block
    assert targets(block) == {
        "HoroAudioRealtimeSafetyHarnessTests", "HoroAudioWatchdogTests", "HoroAudioNullTests",
        "HoroAudioDspTests", "HoroCoreAudioDspTests", "HoroAudioCommandTests",
    }
    assert block.count("--timeout 90 -j 1") == 2
    assert block.count("HoroAudio(CallbackLockPolicyTest|") == 2
    assert "mode: Debug-Release" in block
    assert "max-size: 512M" in block
    assert "exit 1" in block


def test_required_checks_and_sdl_composition_are_preserved() -> None:
    assert "name: Linux / GCC" in job("test")
    assert "name: macOS / Clang" in job("test")
    assert "-DHORO_BUILD_INPUT_SDL3=ON" in job("input-sdl-windows")
    assert "-DHORO_BUILD_AUDIO_SDL3=OFF" in job("input-sdl-windows")
    cleanup = (ROOT / ".github/workflows/cancel-closed-pr.yml").read_text(encoding="utf-8")
    assert "pull_request_target:" in cleanup
    assert "types: [closed]" in cleanup
    assert "actions: write" in cleanup
    assert "ref: ${{ github.sha }}" in cleanup
    assert "persist-credentials: false" in cleanup
