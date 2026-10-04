from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path
from urllib.error import URLError

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("horo_dev_runner", REPOSITORY_ROOT / "scripts" / "dev.py")
assert SPEC is not None
assert SPEC.loader is not None
dev = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dev
SPEC.loader.exec_module(dev)


LOCAL_ENV_FILENAME = ".env.local"


def write_env(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


@pytest.fixture
def subprocess_calls(monkeypatch: pytest.MonkeyPatch) -> list[list[str]]:
    """Capture commands passed through the developer runner subprocess boundary."""
    calls: list[list[str]] = []

    def capture(cmd: Sequence[str], **_: object) -> int:
        calls.append(list(cmd))
        return 0

    monkeypatch.setattr(dev, "execute_subprocess", capture)
    return calls


def test_dotenv_parser_supports_the_declared_non_executable_grammar(tmp_path: Path) -> None:
    env_file = write_env(
        tmp_path / LOCAL_ENV_FILENAME,
        """
        # developer settings
        PLAIN=value
        DOUBLE="two words"
        SINGLE='three words'
        """,
    )

    assert dev.parse_dotenv(env_file) == {
        "PLAIN": "value",
        "DOUBLE": "two words",
        "SINGLE": "three words",
    }


@pytest.mark.parametrize(
    "line",
    [
        "export VALUE=bad",
        "NO_EQUALS",
        "VALUE=two words",
        "VALUE='unterminated",
        "VALUE=$(command)",
        "VALUE=${OTHER}",
        "VALUE=`command`",
    ],
)
def test_dotenv_parser_rejects_shell_syntax_and_malformed_lines(tmp_path: Path, line: str) -> None:
    env_file = write_env(tmp_path / LOCAL_ENV_FILENAME, line)
    with pytest.raises(dev.ConfigurationError):
        dev.parse_dotenv(env_file)


def test_process_environment_overrides_local_configuration(tmp_path: Path) -> None:
    env_file = write_env(
        tmp_path / LOCAL_ENV_FILENAME,
        "HORO_DEV_OTEL_EXPORT=OFF\nHORO_OTEL_ENDPOINT=http://127.0.0.1:4318\n",
    )

    settings = dev.load_settings(
        env_file,
        {
            "HORO_DEV_OTEL_EXPORT": "ON",
            "HORO_OTEL_ENDPOINT": "https://collector.example.com:443",
        },
    )

    assert settings.otel_export_enabled
    assert settings.endpoint.url == "https://collector.example.com:443"


def test_persist_sonar_token_writes_only_configured_local_file(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    env_file = tmp_path / LOCAL_ENV_FILENAME
    env_file.write_text("EXISTING=value\nHORO_SONAR_TOKEN=old\n", encoding="utf-8")
    monkeypatch.setattr(dev, "DEFAULT_ENV_FILE", env_file)

    dev._persist_sonar_token("replacement")

    assert env_file.read_text(encoding="utf-8") == "EXISTING=value\nHORO_SONAR_TOKEN=replacement\n"


def test_disabled_export_ignores_and_later_removes_an_inherited_invalid_endpoint(tmp_path: Path) -> None:
    settings = dev.load_settings(
        tmp_path / "missing.env",
        {
            "HORO_DEV_OTEL_EXPORT": "OFF",
            "HORO_OTEL_ENDPOINT": "not-an-endpoint",
        },
    )

    assert not settings.otel_export_enabled
    assert "HORO_OTEL_ENDPOINT" not in dev.child_environment(
        settings,
        {"HORO_OTEL_EXPORT_APPROVED": "1", "HORO_OTEL_ENDPOINT": "not-an-endpoint"},
    )


@pytest.mark.parametrize(
    "endpoint",
    [
        "127.0.0.1:4318",
        "ftp://127.0.0.1:4318",
        "http://127.0.0.1",
        "http://collector.example.com:4318",
        "https://user:secret@collector.example.com:443",
        "https://collector.example.com:443/custom",
        "https://collector.example.com:443?token=value",
    ],
)
def test_endpoint_validation_rejects_unsafe_or_non_base_urls(endpoint: str) -> None:
    with pytest.raises(dev.ConfigurationError):
        dev.validate_endpoint(endpoint)


def test_endpoint_validation_accepts_supported_base_urls() -> None:
    assert dev.validate_endpoint("http://127.0.0.1:4318/").url == "http://127.0.0.1:4318"
    assert dev.validate_endpoint("http://[::1]:4318").host == "::1"
    assert dev.validate_endpoint("https://collector.example.com:443").port == 443


def test_grafana_url_is_restricted_to_loopback() -> None:
    assert dev.validate_grafana_url("http://127.0.0.1:3000").port == 3000
    assert dev.validate_grafana_url("https://[::1]:3000/").host == "::1"
    with pytest.raises(dev.ConfigurationError):
        dev.validate_grafana_url("https://grafana.example.com:443")


def test_telemetry_switch_controls_build_and_child_environment() -> None:
    endpoint = dev.validate_endpoint("http://127.0.0.1:4318")
    enabled = dev.DeveloperSettings(True, endpoint)
    disabled = dev.DeveloperSettings(False, endpoint)
    inherited = {
        "KEEP": "value",
        "HORO_DEV_OTEL_EXPORT": "OFF",
        "HORO_OTEL_EXPORT_APPROVED": "1",
        "HORO_OTEL_ENDPOINT": "https://unexpected.example.com:443",
    }

    assert "-DHORO_ENABLE_OPENTELEMETRY=ON" in dev.configure_command(enabled)
    assert "-DHORO_ENABLE_OPENTELEMETRY=OFF" in dev.configure_command(disabled)
    assert dev.child_environment(enabled, inherited) == {
        "KEEP": "value",
        "HORO_OTEL_EXPORT_APPROVED": "1",
        "HORO_OTEL_ENDPOINT": "http://127.0.0.1:4318",
    }
    assert dev.child_environment(disabled, inherited) == {"KEEP": "value"}


def test_editor_executable_paths_are_platform_specific(tmp_path: Path) -> None:
    assert dev.editor_executable(tmp_path, "darwin") == tmp_path / "apps/HoroEditor.app/Contents/MacOS/HoroEditor"
    assert dev.editor_executable(tmp_path, "linux") == tmp_path / "apps/HoroEditor"
    assert dev.editor_executable(tmp_path, "win32") == tmp_path / "apps/HoroEditor.exe"


def test_run_editor_configures_builds_and_forwards_arguments_without_a_shell(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    executable = tmp_path / "HoroEditor"
    executable.touch()
    calls: list[tuple[list[str], dict[str, object]]] = []

    def fake_run(arguments: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append((arguments, kwargs))
        return_code = 7 if arguments[0] == str(executable) else 0
        return subprocess.CompletedProcess(arguments, return_code)

    monkeypatch.setattr(dev, "DEFAULT_BUILD_DIRECTORY", tmp_path)
    monkeypatch.setattr(dev, "editor_executable", lambda *args, **kwargs: executable)
    monkeypatch.setattr(dev.subprocess, "run", fake_run)
    monkeypatch.setattr(dev, "collector_is_reachable", lambda endpoint: False)
    settings = dev.DeveloperSettings(True, dev.validate_endpoint("http://127.0.0.1:4318"))

    result = dev.run_editor(settings, ["--project", "path with spaces", "--verbose"])

    assert result == 7
    assert len(calls) == 3
    assert calls[0][0][0] == "cmake"
    assert "-S" in calls[0][0]
    assert calls[1][0][:2] == ["cmake", "--build"]
    assert calls[2][0] == [str(executable), "--project", "path with spaces", "--verbose"]
    assert all("shell" not in keyword_arguments for _, keyword_arguments in calls)
    assert "will continue with local diagnostics only" in capsys.readouterr().err


def test_configure_failure_prevents_build_and_launch(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[list[str]] = []

    def fake_run(arguments: list[str], **_: object) -> subprocess.CompletedProcess[str]:
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 9)

    monkeypatch.setattr(dev.subprocess, "run", fake_run)
    settings = dev.DeveloperSettings(False, dev.validate_endpoint("http://127.0.0.1:4318"))

    assert dev.run_editor(settings, []) == 9
    assert len(calls) == 1


def test_missing_cmake_returns_command_not_found_exit_code(monkeypatch: pytest.MonkeyPatch) -> None:
    def missing_command(*_args: object, **_kwargs: object) -> None:
        raise FileNotFoundError("cmake")

    monkeypatch.setattr(dev.subprocess, "run", missing_command)
    settings = dev.DeveloperSettings(False, dev.validate_endpoint("http://127.0.0.1:4318"))

    assert dev.run_editor(settings, []) == 127


def test_keyboard_interrupt_returns_conventional_exit_code(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    executable = tmp_path / "HoroEditor"
    executable.touch()
    invocation = 0

    def fake_run(arguments: list[str], **_: object) -> subprocess.CompletedProcess[str]:
        nonlocal invocation
        invocation += 1
        if invocation == 3:
            raise KeyboardInterrupt
        return subprocess.CompletedProcess(arguments, 0)

    monkeypatch.setattr(dev, "DEFAULT_BUILD_DIRECTORY", tmp_path)
    monkeypatch.setattr(dev, "editor_executable", lambda *args, **kwargs: executable)
    monkeypatch.setattr(dev.subprocess, "run", fake_run)
    settings = dev.DeveloperSettings(False, dev.validate_endpoint("http://127.0.0.1:4318"))

    assert dev.run_editor(settings, []) == 130


def test_cli_run_supports_all_argument_variations(monkeypatch: pytest.MonkeyPatch) -> None:
    forwarded: list[list[str]] = []
    settings = dev.DeveloperSettings(False, dev.validate_endpoint("http://127.0.0.1:4318"))
    monkeypatch.setattr(dev, "load_settings", lambda: settings)

    def fake_run_editor(_: object, arguments: list[str]) -> int:
        forwarded.append(arguments)
        return 0

    monkeypatch.setattr(dev, "run_editor", fake_run_editor)

    assert dev.main(["run", "editor", "--", "--project", "foo"]) == 0
    assert forwarded[-1] == ["--project", "foo"]

    assert dev.main(["run", "--", "--headless"]) == 0
    assert forwarded[-1] == ["--headless"]

    assert dev.main(["run", "--headless"]) == 0
    assert forwarded[-1] == ["--headless"]

    assert dev.main(["run", "editor"]) == 0
    assert forwarded[-1] == []



def test_grafana_sync_skips_an_unchanged_dashboard(monkeypatch: pytest.MonkeyPatch) -> None:
    endpoint = dev.validate_grafana_url("http://127.0.0.1:3000")
    source_tag = dev._grafana_source_tag(dev.GRAFANA_DASHBOARD_PATH)
    responses = iter([{"database": "ok"}, {"dashboard": {"tags": [source_tag]}}])
    requests: list[object] = []

    def fake_read_json(request: object, _timeout: float) -> dict[str, object]:
        requests.append(request)
        return next(responses)

    monkeypatch.setattr(dev, "_read_json", fake_read_json)

    assert dev.sync_grafana_dashboard(endpoint) == "current"
    assert len(requests) == 2


def test_grafana_sync_updates_a_stale_dashboard(monkeypatch: pytest.MonkeyPatch) -> None:
    endpoint = dev.validate_grafana_url("http://127.0.0.1:3000")
    responses = iter([{"database": "ok"}, {"dashboard": {"tags": ["old"]}}, {"status": "success"}])
    requests: list[object] = []

    def fake_read_json(request: object, _timeout: float) -> dict[str, object]:
        requests.append(request)
        return next(responses)

    monkeypatch.setattr(dev, "_read_json", fake_read_json)

    assert dev.sync_grafana_dashboard(endpoint) == "updated"
    payload = json.loads(requests[-1].data.decode("utf-8"))
    assert payload["overwrite"] is True
    assert any(tag.startswith(dev.GRAFANA_SOURCE_TAG_PREFIX) for tag in payload["dashboard"]["tags"])


def test_grafana_unavailability_never_blocks_startup(monkeypatch: pytest.MonkeyPatch) -> None:
    endpoint = dev.validate_grafana_url("http://127.0.0.1:3000")

    def unavailable(_request: object, _timeout: float) -> dict[str, object]:
        raise URLError("offline")

    monkeypatch.setattr(dev, "_read_json", unavailable)
    assert dev.sync_grafana_dashboard(endpoint) == "unavailable"


def test_doctor_report_aggregation_and_rendering() -> None:
    report = dev.DoctorReport()
    report.add("Toolchain", "C++ Compiler", dev.CheckStatus.OK, "clang 17")
    report.add("Observability", "Collector", dev.CheckStatus.WARN, "unreachable")
    report.add("Toolchain", "CMake", dev.CheckStatus.ERROR, "missing")

    assert report.has_errors is True
    assert report.has_warnings is True

    serialized = report.to_dict()
    assert serialized["has_errors"] is True
    assert len(serialized["checks"]) == 3

    rendered = report.render(use_color=False)
    assert "[✓] C++ Compiler: clang 17" in rendered
    assert "[!] Collector: unreachable" in rendered
    assert "[✗] CMake: missing" in rendered
    assert "Critical issues found" in rendered


def test_run_doctor_collects_valid_checks() -> None:
    settings = dev.DeveloperSettings(False, dev.validate_endpoint("http://127.0.0.1:4318"))
    report = dev.run_doctor(settings)
    assert len(report.items) >= 5
    categories = {item.category for item in report.items}
    assert "Toolchain" in categories
    assert "Graphics" in categories or "Platform" in categories


def test_main_doctor_command_json_and_exit_code(capsys: pytest.CaptureFixture[str]) -> None:
    exit_code = dev.main(["doctor", "--json"])
    assert exit_code in (0, 1)
    captured = capsys.readouterr()
    parsed = json.loads(captured.out)
    assert "checks" in parsed
    assert isinstance(parsed["checks"], list)


def test_main_build_command_invokes_cmake_build(subprocess_calls: list[list[str]], tmp_path: Path) -> None:
    exit_code = dev.main(
        [
            "build",
            "HoroEditorUiComponentsRenderTests",
            "-B",
            str(tmp_path),
            "--compiler-launcher",
            "sccache",
            "--msvc-debug-information-format",
            "Embedded",
        ]
    )
    assert exit_code == 0
    assert len(subprocess_calls) == 2
    assert subprocess_calls[0][0] == "cmake"
    assert "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache" in subprocess_calls[0]
    assert "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded" in subprocess_calls[0]
    assert subprocess_calls[1][:4] == ["cmake", "--build", str(tmp_path), "--target"]
    assert subprocess_calls[1][4] == "HoroEditorUiComponentsRenderTests"


def test_main_test_command_filters_and_invokes_ctest(subprocess_calls: list[list[str]], tmp_path: Path) -> None:
    exit_code = dev.main(["test", "SceneDocument", "-E", "Slow", "-B", str(tmp_path)])
    assert exit_code == 0
    # configure + build + ctest
    assert len(subprocess_calls) == 3
    ctest_call = subprocess_calls[2]
    assert ctest_call[0] == "ctest"
    assert "-R" in ctest_call
    assert "SceneDocument" in ctest_call
    assert "-E" in ctest_call
    assert "Slow" in ctest_call
    assert "-LE" in ctest_call
    assert "gui" in ctest_call


def test_main_test_command_forwards_windows_cache_configuration_to_cmake(
    subprocess_calls: list[list[str]], tmp_path: Path
) -> None:
    assert (
        dev.main(
            [
                "test",
                "-B",
                str(tmp_path),
                "--compiler-launcher",
                "sccache",
                "--msvc-debug-information-format",
                "Embedded",
            ]
        )
        == 0
    )
    assert "-DCMAKE_C_COMPILER_LAUNCHER=sccache" in subprocess_calls[0]
    assert "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache" in subprocess_calls[0]
    assert "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded" in subprocess_calls[0]


def test_main_check_command_invokes_full_ci_pass(subprocess_calls: list[list[str]], tmp_path: Path) -> None:
    exit_code = dev.main(
        [
            "check",
            "-B",
            str(tmp_path),
            "--compiler-launcher",
            "sccache",
            "--msvc-debug-information-format",
            "Embedded",
        ]
    )
    assert exit_code == 0
    assert len(subprocess_calls) == 3
    assert subprocess_calls[0][0] == "cmake"
    assert "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache" in subprocess_calls[0]
    assert "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded" in subprocess_calls[0]
    assert subprocess_calls[1][:3] == ["cmake", "--build", str(tmp_path)]
    assert subprocess_calls[2][0] == "ctest"


def test_is_formattable_filters_cpp_extensions_and_ignores_vendor() -> None:
    assert dev._is_formattable(Path("src/editor/main.cpp")) is True
    assert dev._is_formattable(Path("include/Horo/Engine.h")) is True
    assert dev._is_formattable(Path("vendor/imgui/imgui.cpp")) is False
    assert dev._is_formattable(Path("build/generated.cpp")) is False
    assert dev._is_formattable(Path("scripts/dev.py")) is False


def test_format_exclusions_apply_inside_the_repository(monkeypatch, tmp_path) -> None:
    root = tmp_path / "build" / "checkout"
    monkeypatch.setattr(dev, "REPOSITORY_ROOT", root)
    assert dev._is_formattable(root / "src" / "main.cpp") is True
    assert dev._is_formattable(root / "build" / "generated.cpp") is False
    assert dev._is_formattable(root / "vendor" / "external.cpp") is False
    assert dev._is_formattable(tmp_path / "outside.cpp") is False


def test_main_format_command_invokes_clang_format(
    monkeypatch: pytest.MonkeyPatch, subprocess_calls: list[list[str]], tmp_path: Path
) -> None:
    test_cpp = tmp_path / "Test.cpp"
    test_cpp.write_text("int main() { return 0; }\n", encoding="utf-8")

    monkeypatch.setattr(dev.shutil, "which", lambda cmd: "/usr/bin/clang-format" if cmd == "clang-format" else None)

    exit_code = dev.main(["format", str(test_cpp)])
    assert exit_code == 0
    assert len(subprocess_calls) == 1
    assert subprocess_calls[0][:2] == ["/usr/bin/clang-format", "-i"]
    assert str(test_cpp) in subprocess_calls[0]


def test_staged_format_refuses_to_stage_unstaged_worktree_changes(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run(["git", "config", "user.email", "tests@horo.local"], cwd=tmp_path, check=True)
    subprocess.run(["git", "config", "user.name", "Horo Tests"], cwd=tmp_path, check=True)
    source = tmp_path / "Partial.cpp"
    source.write_text("int value = 0;\n", encoding="utf-8")
    subprocess.run(["git", "add", "Partial.cpp"], cwd=tmp_path, check=True)
    subprocess.run(["git", "commit", "-qm", "initial"], cwd=tmp_path, check=True)

    source.write_text("int value = 1;\n", encoding="utf-8")
    subprocess.run(["git", "add", "Partial.cpp"], cwd=tmp_path, check=True)
    staged_contents = subprocess.run(
        ["git", "show", ":Partial.cpp"], cwd=tmp_path, capture_output=True, text=True, check=True
    ).stdout
    source.write_text("int value = 2;\n", encoding="utf-8")

    format_calls: list[list[str]] = []
    monkeypatch.setattr(dev, "REPOSITORY_ROOT", tmp_path)
    monkeypatch.setattr(dev, "_find_clang_format", lambda: "/usr/bin/clang-format")
    monkeypatch.setattr(dev, "_apply_format_chunk", lambda _binary, chunk, _check: format_calls.append(chunk) or 0)

    assert dev.run_format(staged=True) == 1
    assert format_calls == []
    assert "refusing to format partially staged files" in capsys.readouterr().err
    assert subprocess.run(
        ["git", "show", ":Partial.cpp"], cwd=tmp_path, capture_output=True, text=True, check=True
    ).stdout == staged_contents


def test_doctor_runs_gracefully_with_broken_env_file(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    broken_env = write_env(tmp_path / LOCAL_ENV_FILENAME, "INVALID-ENV-LINE\n")
    monkeypatch.setattr(dev, "DEFAULT_ENV_FILE", broken_env)

    exit_code = dev.main(["doctor", "--json"])
    assert exit_code == 1
    captured = capsys.readouterr()
    report = json.loads(captured.out)
    assert report["has_errors"] is True
    config_check = next(c for c in report["checks"] if c["category"] == "Configuration")
    assert config_check["status"] == "ERROR"
    assert "Syntax/validation error" in config_check["message"]


def test_gui_flag_propagates_to_cmake_configure(subprocess_calls: list[list[str]], tmp_path: Path) -> None:
    exit_code = dev.main(["test", "--gui", "-B", str(tmp_path)])
    assert exit_code == 0
    assert len(subprocess_calls) == 3
    # Configure command must have HORO_ENABLE_IMGUI_UI_TESTS=ON
    cmake_cfg = subprocess_calls[0]
    assert "-DHORO_ENABLE_IMGUI_UI_TESTS=ON" in cmake_cfg


@pytest.mark.parametrize("command", ["build", "test", "check"])
def test_fetchcontent_directory_reaches_cmake_as_one_argument(
    command: str, subprocess_calls: list[list[str]], tmp_path: Path
) -> None:
    dependencies = tmp_path / "dependencies with spaces türkçe"
    assert dev.main([command, "-B", str(tmp_path), "--fetchcontent-base-dir", str(dependencies)]) == 0
    assert f"-DFETCHCONTENT_BASE_DIR={dependencies.resolve()}" in subprocess_calls[0]


@pytest.mark.parametrize("launcher,debug_format", [(None, None), ("ccache", None), ("sccache", "Embedded")])
def test_compiler_cache_options_configure_both_languages(launcher: str | None, debug_format: str | None) -> None:
    arguments = dev._compiler_cache_arguments(launcher, debug_format)
    expected = []
    if launcher:
        expected.extend([
            f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}", f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}",
        ])
    if debug_format:
        expected.append(f"-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT={debug_format}")
    assert arguments == expected


def test_fetchcontent_default_preserves_explicit_cmake_override(
    subprocess_calls: list[list[str]], tmp_path: Path
) -> None:
    override = "-DFETCHCONTENT_BASE_DIR=explicit-override"
    arguments = [override]
    assert dev.run_build(build_directory=tmp_path, fetchcontent_base_dir=tmp_path / "default", extra_cmake_args=arguments) == 0
    configure = subprocess_calls[0]
    assert configure.index(f"-DFETCHCONTENT_BASE_DIR={(tmp_path / 'default').resolve()}") < configure.index(override)
    assert arguments == [override]


def test_skip_build_preserves_ctest_filters_junit_and_failure(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    (tmp_path / "CTestTestfile.cmake").touch()
    junit = tmp_path / "results with spaces" / "ctest.xml"
    calls: list[list[str]] = []

    def fail_ctest(command: Sequence[str], **_: object) -> int:
        calls.append(list(command))
        return 8

    monkeypatch.setattr(dev, "execute_subprocess", fail_ctest)
    assert dev.main([
        "test", "--skip-build", "-B", str(tmp_path), "-R", "Audio", "-E", "Slow", "--junit", str(junit)
    ]) == 8
    assert calls == [[
        "ctest", "--test-dir", str(tmp_path), "--output-on-failure", "--no-tests=error",
        "-LE", "gui", "-R", "Audio", "-E", "Slow", "--output-junit", str(junit),
    ]]
    assert junit.parent.is_dir()


def test_skip_build_rejects_missing_test_configuration(
    subprocess_calls: list[list[str]], tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert dev.main(["test", "--skip-build", "-B", str(tmp_path)]) == 2
    assert subprocess_calls == []
    assert "requires a configured test directory" in capsys.readouterr().err


def test_grafana_sync_passes_authorization_header(monkeypatch: pytest.MonkeyPatch) -> None:
    endpoint = dev.validate_grafana_url("http://127.0.0.1:3000")
    source_tag = dev._grafana_source_tag(dev.GRAFANA_DASHBOARD_PATH)
    responses = iter([{"database": "ok"}, {"dashboard": {"tags": [source_tag]}}])
    requests: list[Request] = []

    def fake_read_json(request: Request, _timeout: float) -> dict[str, object]:
        requests.append(request)
        return next(responses)

    monkeypatch.setattr(dev, "_read_json", fake_read_json)
    status = dev.sync_grafana_dashboard(endpoint, api_token="secret_token_123")
    assert status == "current"
    assert requests[0].headers.get("Authorization") == "Bearer secret_token_123"


def test_main_help_command_and_empty_arguments(capsys: pytest.CaptureFixture[str]) -> None:
    assert dev.main([]) == 0
    out_empty = capsys.readouterr().out
    assert "usage:" in out_empty
    assert "doctor" in out_empty

    assert dev.main(["help"]) == 0
    out_help = capsys.readouterr().out
    assert "usage:" in out_help

    assert dev.main(["help", "doctor"]) == 0
    out_doc = capsys.readouterr().out
    assert "--json" in out_doc

    assert dev.main(["help", "nonexistent"]) == 2
    err = capsys.readouterr().err
    assert "unknown command 'nonexistent'" in err
