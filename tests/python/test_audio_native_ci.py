"""Keep real native adapter translation units in each platform's audio lane."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def test_release_audio_compiles_each_host_peer_and_retains_boundary_results() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    configure = workflow.split("- name: Configure Release audio checks", 1)[1].split("\n      - name:", 1)[0]
    for peer, host in (("WASAPI", "Windows"), ("COREAUDIO", "macOS"), ("PIPEWIRE", "Linux")):
        assert f"-DHORO_BUILD_AUDIO_{peer}=${{{{ runner.os == '{host}' && 'ON' || 'OFF' }}}}" in configure
    assert "--target HoroAudioNativeFactoryTests HoroAudioComponentPreviewHostTests HoroAudioEditorPreviewTests --parallel 2" in workflow
    test = workflow.split("- name: Test native audio adapter boundaries", 1)[1].split("\n      - name:", 1)[0]
    assert "--no-tests=error" in test
    assert "'^(HoroAudioNativeFactoryTests|HoroAudioComponentPreviewHostTests|HoroAudioEditorPreviewTests)::'" in test
    assert "continue-on-error" not in test
    assert "            build/ci-audio-release/native-audio-ctest.xml" in workflow
    packages = (ROOT / ".github/scripts/install_linux_build_dependencies.sh").read_text(encoding="utf-8")
    assert "libpipewire-0.3-dev" in packages.split("packages=(", 1)[1].split(")", 1)[0]


def test_factory_executable_links_real_native_targets() -> None:
    cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    block = cmake.split("if(TARGET HoroAudioWasapi OR TARGET HoroAudioCoreAudio OR TARGET HoroAudioPipeWire)", 1)[1]
    block = block.split("add_executable(HoroAudioWatchdogTests", 1)[0]
    assert "foreach(peer Wasapi CoreAudio PipeWire)" in block
    assert "target_link_libraries(HoroAudioNativeFactoryTests PRIVATE HoroAudio${peer})" in block
    for peer in ("WASAPI", "COREAUDIO", "PIPEWIRE"):
        assert re.search(rf"HORO_TEST_{peer}=\$<BOOL:\$\{{HORO_BUILD_AUDIO_{peer}\}}>", block)


def test_preview_composition_uses_native_peers_in_product_and_boundary_tests() -> None:
    product = (ROOT / "apps/HoroEditor/cmake/Host.cmake").read_text(encoding="utf-8")
    tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    frontend = tests.split("add_executable(HoroAudioEditorPreviewTests", 1)[1]
    frontend = frontend.split("horo_register_catch_test(HoroAudioEditorPreviewTests", 1)[0]
    assert "unit/runtime/audio/NativeAudioFrontendHandoffTests.cpp" in frontend
    assert "HoroAudioNativeRuntime" in frontend
    boundary = tests.split("add_executable(HoroAudioComponentPreviewHostTests", 1)[1]
    boundary = boundary.split("horo_register_catch_test(HoroAudioComponentPreviewHostTests", 1)[0]
    assert "unit/editor/EditorAudioPreviewSelectionTests.cpp" in boundary
    for source, target in ((product, "HoroEditor"), (boundary, "HoroAudioComponentPreviewHostTests")):
        loop = source.split("foreach(audio_peer_target IN ITEMS HoroAudioWasapi HoroAudioCoreAudio HoroAudioPipeWire)", 1)[1]
        loop = loop.split("endforeach()", 1)[0]
        assert f"target_link_libraries({target} PRIVATE ${{audio_peer_target}})" in loop
        for installed in (0, 1):
            assert f"target_compile_definitions({target} PRIVATE HORO_EDITOR_AUDIO_${{audio_peer_macro}}={installed})" in loop


def test_native_preview_control_owner_is_compiled_in_product_and_boundary_tests() -> None:
    product = (ROOT / "apps/HoroEditor/cmake/Host.cmake").read_text(encoding="utf-8")
    tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    boundary = tests.split("add_executable(HoroAudioComponentPreviewHostTests", 1)[1]
    boundary = boundary.split("horo_register_catch_test(HoroAudioComponentPreviewHostTests", 1)[0]
    assert "unit/editor/EditorAudioPreviewControlHostTests.cpp" in boundary
    for source, target in ((product, "HoroEditor"), (boundary, "HoroAudioComponentPreviewHostTests")):
        assert "HoroEditor/app/EditorAudioPreviewControlHost.cpp" in source
        assert f"target_link_libraries({target} PRIVATE Threads::Threads)" in source
