from __future__ import annotations

import importlib.util
import json
import subprocess
import struct
import sys
from collections.abc import Sequence
from contextlib import contextmanager, nullcontext
from pathlib import Path

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("horo_dev_runner", REPOSITORY_ROOT / "scripts" / "dev.py")
assert SPEC is not None
assert SPEC.loader is not None
dev = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dev
SPEC.loader.exec_module(dev)


LOCAL_ENV_FILENAME = ".env.local"


@pytest.mark.parametrize("cancellation", [KeyboardInterrupt(), SystemExit(23)])
def test_cleanup_cancellation_blocks_later_builds_and_preserves_exit(
    monkeypatch: pytest.MonkeyPatch, cancellation: BaseException,
) -> None:
    recorded: list[Path] = []

    def interrupted(*_, **__):
        raise cancellation from dev.CleanupError("cleanup unconfirmed")

    monkeypatch.setattr(dev, 'run', interrupted)
    monkeypatch.setattr(dev, 'record_cleanup_failure', recorded.append)
    if isinstance(cancellation, SystemExit):
        with pytest.raises(SystemExit) as caught:
            dev.execute_subprocess([sys.executable])
        assert caught.value is cancellation
        assert caught.value.code == 23
    else:
        assert dev.execute_subprocess([sys.executable]) == 130
    assert recorded == [dev.REPOSITORY_ROOT]


def test_sonar_property_values_stay_single_arguments_and_extra_flags_are_explicit(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path,
) -> None:
    commands: list[list[str]] = []
    compile_commands = tmp_path / "commands with spaces türkçe -Dother=value.json"
    compile_commands.write_text("[]", encoding="utf-8")
    scanner = tmp_path / "scanner"
    property_value = "fixture value -Dother=value"
    pr_key = "3410 -Dother=value"
    extra = ["-X", "-Dsonar.verbose=true"]
    monkeypatch.setattr(dev, '_resolve_sonar_token', lambda *_: property_value)
    monkeypatch.setattr(dev, '_find_sonar_scanner', lambda: scanner)

    def capture(command, **_):
        commands.append(list(command))
        return 0

    monkeypatch.setattr(dev, 'execute_subprocess', capture)
    assert dev.run_sonar(token=property_value, pr_key=pr_key,
                         compile_commands=compile_commands, extra_sonar_args=extra) == 0
    assert commands == [[str(scanner), f"-Dsonar.token={property_value}",
                         f"-Dsonar.cfamily.compile-commands={compile_commands}",
                         f"-Dsonar.pullrequest.key={pr_key}", *extra]]


@pytest.fixture(autouse=True)
def isolated_cache(monkeypatch: pytest.MonkeyPatch) -> None:
    """Ordinary runner unit tests neither create host caches nor wait for real builds."""
    monkeypatch.setattr(dev, 'heavy_build_lock', lambda _: nullcontext())
    monkeypatch.setattr(dev, 'automatic_cache', lambda _: nullcontext(None))


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

    monkeypatch.setattr(dev, 'execute_subprocess', capture)
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


def test_persist_sonar_token_writes_only_configured_local_file(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    env_file = tmp_path / LOCAL_ENV_FILENAME
    env_file.write_text("EXISTING=value\nHORO_SONAR_TOKEN=old\n", encoding="utf-8")
    monkeypatch.setattr(dev, 'DEFAULT_ENV_FILE', env_file)

    dev._persist_sonar_token("replacement")

    assert env_file.read_text(encoding="utf-8") == "EXISTING=value\nHORO_SONAR_TOKEN=replacement\n"


def test_editor_executable_paths_are_platform_specific(tmp_path: Path) -> None:
    assert dev.editor_executable(tmp_path, "darwin") == tmp_path / "apps/HoroEditor.app/Contents/MacOS/HoroEditor"
    assert dev.editor_executable(tmp_path, "linux") == tmp_path / "apps/HoroEditor"
    assert dev.editor_executable(tmp_path, "win32") == tmp_path / "apps/HoroEditor.exe"


def test_editor_launch_stays_local_with_obsolete_export_settings(monkeypatch, tmp_path, capsys):
    monkeypatch.setenv("HORO_DEV_OTEL_EXPORT", "ON")
    monkeypatch.setenv("HORO_OTEL_ENDPOINT", "invalid legacy endpoint")
    monkeypatch.setenv("HORO_OTEL_EXPORT_APPROVED", "1")
    env_file = write_env(tmp_path / LOCAL_ENV_FILENAME, "not executable shell configuration")
    monkeypatch.setattr(dev, "DEFAULT_ENV_FILE", env_file)
    executable = tmp_path / "HoroEditor"
    executable.touch()
    monkeypatch.setattr(dev, "editor_executable", lambda *_: executable)
    calls = []
    monkeypatch.setattr(dev, "execute_subprocess", lambda command, **kwargs: calls.append(list(command)) or 0)
    assert dev.main(["run", "editor"]) == 0
    assert "-DHORO_ENABLE_TELEMETRY=ON" in calls[0]
    assert not any("OPENTELEMETRY" in argument for argument in calls[0])
    output = capsys.readouterr().out
    assert "LOCAL" in output
    assert "Collector" not in output
    assert "OpenTelemetry" not in output


@pytest.mark.parametrize("live_ui", [False, True])
@pytest.mark.parametrize("target_platform", ["win32", "darwin"])
def test_development_theme_and_live_ui_are_explicit_launch_options(monkeypatch, tmp_path, live_ui, target_platform, capsys):
    theme = tmp_path / "theme with spaces.json"
    theme.write_text('{"colors":{"WindowBg":"#112233"}}', encoding="utf-8")
    executable = tmp_path / "HoroEditor.exe"
    executable.touch()
    calls = []
    monkeypatch.setattr(dev.sys, "platform", target_platform)
    monkeypatch.setattr(dev, "editor_executable", lambda *_: executable)
    monkeypatch.setattr(dev, "execute_subprocess", lambda command, **kwargs: calls.append((list(command), kwargs)) or 0)
    assert dev.run_editor([], tmp_path, theme_file=theme, live_ui=live_ui) == 0
    assert f"-DHORO_EDITOR_LIVE_UI={'ON' if live_ui else 'OFF'}" in calls[0][0]
    assert calls[-1][1]["env"]["HORO_THEME_OVERRIDE"] == str(theme.resolve())
    assert calls[-1][0] == [str(executable)]
    output = capsys.readouterr().out
    assert ("ON / RCC++" in output) is live_ui
    assert "ON / JSON" in output
    assert "[3/3] LAUNCH" in output
    assert "\033[" not in output


def test_live_ui_rejects_unsupported_platform_before_build(monkeypatch, capsys):
    monkeypatch.setattr(dev.sys, "platform", "linux")
    monkeypatch.setattr(dev, "_configure_and_build", lambda *_args, **_kwargs: pytest.fail("unexpected build"))
    assert dev.run_editor([], live_ui=True) == 2
    assert "macOS/AppleClang" in capsys.readouterr().err


def test_missing_development_theme_stops_before_configure(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(dev, "execute_subprocess", lambda *_args, **_kwargs: pytest.fail("unexpected build"))
    assert dev.run_editor([], theme_file=tmp_path / "missing.json") == 2
    assert "theme file not found" in capsys.readouterr().err


def test_run_parser_keeps_development_flags_out_of_editor_arguments(monkeypatch, tmp_path):
    calls = []
    monkeypatch.setattr(dev, "run_editor", lambda arguments, **kwargs: calls.append((arguments, kwargs)) or 0)
    assert dev.main(["run", "editor", "--theme-file", str(tmp_path / "theme.json"), "--live-ui", "--", "--exit-after-first-frame"]) == 0
    assert calls == [(["--exit-after-first-frame"], {"theme_file": tmp_path / "theme.json", "live_ui": True})]


def test_run_editor_configures_builds_and_forwards_arguments_without_a_shell(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    executable = tmp_path / "HoroEditor"
    executable.touch()
    calls: list[tuple[list[str], dict[str, object]]] = []

    def fake_run(arguments: list[str], **kwargs: object) -> int:
        calls.append((arguments, kwargs))
        return_code = 7 if arguments[0] == str(executable) else 0
        return return_code

    monkeypatch.setattr(dev, 'DEFAULT_BUILD_DIRECTORY', tmp_path)
    monkeypatch.setattr(dev, 'editor_executable', lambda *args, **kwargs: executable)
    monkeypatch.setattr(dev, 'run', fake_run)

    result = dev.run_editor(["--project", "path with spaces", "--verbose"])

    assert result == 7
    assert len(calls) == 3
    assert calls[0][0][0] == "cmake"
    assert "-S" in calls[0][0]
    assert calls[1][0][:2] == ["cmake", "--build"]
    assert calls[2][0] == [str(executable), "--project", "path with spaces", "--verbose"]
    assert all("shell" not in keyword_arguments for _, keyword_arguments in calls)
    assert "Collector" not in capsys.readouterr().out


def test_configure_failure_prevents_build_and_launch(monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    calls: list[list[str]] = []

    def fake_run(arguments: list[str], **_: object) -> int:
        calls.append(arguments)
        return 9

    monkeypatch.setattr(dev, 'run', fake_run)

    assert dev.run_editor([]) == 9
    assert len(calls) == 1
    output = capsys.readouterr().out
    assert "[FAIL 9] Configure" in output
    assert "[2/3] BUILD" not in output
    assert "[3/3] LAUNCH" not in output


def test_missing_cmake_returns_command_not_found_exit_code(monkeypatch: pytest.MonkeyPatch) -> None:
    def missing_command(*_args: object, **_kwargs: object) -> None:
        raise FileNotFoundError("cmake")

    monkeypatch.setattr(dev, 'run', missing_command)

    assert dev.run_editor([]) == 127


def test_keyboard_interrupt_returns_conventional_exit_code(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    executable = tmp_path / "HoroEditor"
    executable.touch()
    invocation = 0

    def fake_run(arguments: list[str], **_: object) -> int:
        nonlocal invocation
        invocation += 1
        if invocation == 3:
            raise KeyboardInterrupt
        return 0

    monkeypatch.setattr(dev, 'DEFAULT_BUILD_DIRECTORY', tmp_path)
    monkeypatch.setattr(dev, 'editor_executable', lambda *args, **kwargs: executable)
    monkeypatch.setattr(dev, 'run', fake_run)

    assert dev.run_editor([]) == 130


def test_cli_run_supports_all_argument_variations(monkeypatch: pytest.MonkeyPatch) -> None:
    forwarded: list[list[str]] = []

    def fake_run_editor(arguments: list[str], **options: object) -> int:
        assert options == {"theme_file": None, "live_ui": False}
        forwarded.append(arguments)
        return 0

    monkeypatch.setattr(dev, 'run_editor', fake_run_editor)

    assert dev.main(["run", "editor", "--", "--project", "foo"]) == 0
    assert forwarded[-1] == ["--project", "foo"]

    assert dev.main(["run", "--", "--headless"]) == 0
    assert forwarded[-1] == ["--headless"]

    assert dev.main(["run", "--headless"]) == 0
    assert forwarded[-1] == ["--headless"]

    assert dev.main(["run", "editor"]) == 0
    assert forwarded[-1] == []


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
    report = dev.run_doctor()
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


def test_main_cache_lease_covers_configure_and_build(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    events: list[str] = []
    environment = {"CCACHE_CONFIGPATH": "ready.conf"}
    binding = dev.CacheBinding("/tools/ccache", environment, "gen-ready", "a" * 40)

    @contextmanager
    def lease(_: Path):
        events.append("acquire")
        yield binding
        events.append("release")

    def execute(command: Sequence[str], **kwargs: object) -> int:
        assert events[-1] != "release"
        assert kwargs["env"] is environment
        events.append("configure" if "-S" in command else "build")
        if "-S" in command:
            assert "-DCMAKE_CXX_COMPILER_LAUNCHER=/tools/ccache" in command
            assert "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON" in command
        return 0

    monkeypatch.setattr(dev, 'automatic_cache', lease)
    monkeypatch.setattr(dev, 'execute_subprocess', execute)
    for key in list(dev.os.environ):
        if key.startswith(("CCACHE_", "SCCACHE_")):
            monkeypatch.delenv(key)
    assert dev.run_build(build_directory=tmp_path) == 0
    assert events == ["acquire", "configure", "build", "release"]


def test_main_cache_releases_after_failed_configure(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    events: list[str] = []

    @contextmanager
    def lease(_: Path):
        try:
            events.append("acquire")
            yield dev.CacheBinding("ccache", {}, "ready", "a" * 40)
        finally:
            events.append("release")

    monkeypatch.setattr(dev, 'automatic_cache', lease)
    monkeypatch.setattr(dev, '_uses_explicit_cache', lambda *_: False)
    monkeypatch.setattr(dev, 'execute_subprocess', lambda *_args, **_kwargs: 7)
    assert dev.run_build(build_directory=tmp_path) == 7
    assert events == ["acquire", "release"]


@pytest.mark.parametrize("arguments", [
    ["--compiler-launcher", "sccache"], ["--no-cache"],
    ["--cmake-arg=-DCMAKE_CXX_COMPILER_LAUNCHER=custom"],
    ["--cmake-arg=-DHORO_ENGINE_ENABLE_PCH=ON"],
    ["--cmake-arg=-DHORO_ENGINE_ENABLE_PCH=YES"],
    ["--cmake-arg=-DHORO_ENGINE_ENABLE_PCH=2"],
    ["--cmake-arg=-DCMAKE_DISABLE_PRECOMPILE_HEADERS=OFF"],
])
def test_explicit_cache_configuration_bypasses_discovery(
    arguments: list[str], monkeypatch: pytest.MonkeyPatch, subprocess_calls: list[list[str]], tmp_path: Path
) -> None:
    def unexpected(_: Path):
        pytest.fail("explicit cache configuration must not discover a managed cache")

    monkeypatch.setattr(dev, 'automatic_cache', unexpected)
    assert dev.main(["build", "-B", str(tmp_path), *arguments]) == 0
    assert len(subprocess_calls) == 2


def test_cache_prepare_compiles_isolated_main_with_matching_build_settings(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    source, build = tmp_path / "main source", tmp_path / "main source" / "build" / "dev-cache"
    calls: list[tuple[list[str], object, object]] = []
    binding = dev.CacheBinding("ccache", {"CACHE": "main"}, "candidate", "b" * 40)

    def prepare(repo: Path, callback, **kwargs: object) -> int:
        assert repo == dev.REPOSITORY_ROOT
        return callback(source, build, binding)

    def execute(command: Sequence[str], **kwargs: object) -> int:
        calls.append((list(command), kwargs.get("cwd"), kwargs.get("env")))
        return 0

    monkeypatch.setattr(dev, 'prepare_cache', prepare)
    monkeypatch.setattr(dev, 'status', lambda _: {"ready": True})
    monkeypatch.setattr(dev, 'execute_subprocess', execute)
    assert dev.main(["cache", "prepare", "--offline", "--target", "HoroCore", "-j", "1"]) == 0
    assert len(calls) == 2
    assert calls[0][0][2] == str(source)
    assert calls[1][0][-2:] == ["--parallel", "1"]
    assert all(cwd == source and env is binding.env for _, cwd, env in calls)


def test_debug_cache_normalization_preserves_flags_and_handles_spaces(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setattr(dev.sys, 'platform', "linux")
    build = tmp_path / "build with spaces"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("CMAKE_CXX_FLAGS:STRING=-Wall -Wextra\n")
    source = tmp_path / "source with spaces"
    arguments = dev._cache_configure_arguments(["cmake", "-DCMAKE_C_FLAGS=-O1"], build, source)
    cxx = next(arg.split("=", 1)[1] for arg in arguments if arg.startswith("-DCMAKE_CXX_FLAGS="))
    assert dev.shlex.split(cxx) == ["-Wall", "-Wextra", f"-fdebug-prefix-map={source}=.", f"-fdebug-prefix-map={build}=."]
    c_flags = [arg for arg in arguments if arg.startswith("-DCMAKE_C_FLAGS=")][-1]
    assert "-O1" in c_flags
    assert dev._cache_configure_arguments(arguments, build, source)[-1] == arguments[-1]


def test_build_job_count_must_be_positive() -> None:
    with pytest.raises(SystemExit):
        dev.main(["build", "--jobs", "0"])


@pytest.mark.parametrize("argument", ["-S/other-source", "-B/other-build", "--preset=other", "-C/other.cmake", "../other"])
def test_cache_prepare_rejects_topology_overrides_before_fetch(argument: str, subprocess_calls: list[list[str]]) -> None:
    assert dev.main(["cache", "prepare", f"--cmake-arg={argument}"]) == 2
    assert subprocess_calls == []


def test_unconfirmed_process_cleanup_records_build_block(monkeypatch: pytest.MonkeyPatch) -> None:
    recorded: list[Path] = []

    def fail(*_args: object, **_kwargs: object) -> int:
        raise dev.CleanupError("unconfirmed descendants")

    monkeypatch.setattr(dev, 'run', fail)
    monkeypatch.setattr(dev, 'record_cleanup_failure', recorded.append)
    assert dev.execute_subprocess(["fixture"]) == 126
    assert recorded == [dev.REPOSITORY_ROOT]


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
    monkeypatch.setattr(dev, 'REPOSITORY_ROOT', root)
    assert dev._is_formattable(root / "src" / "main.cpp") is True
    assert dev._is_formattable(root / "build" / "generated.cpp") is False
    assert dev._is_formattable(root / "vendor" / "external.cpp") is False
    assert dev._is_formattable(tmp_path / "outside.cpp") is False


def test_main_format_command_invokes_clang_format(
    monkeypatch: pytest.MonkeyPatch, subprocess_calls: list[list[str]], tmp_path: Path
) -> None:
    test_cpp = tmp_path / "Test.cpp"
    test_cpp.write_text("int main() { return 0; }\n", encoding="utf-8")

    monkeypatch.setattr(dev.shutil, 'which', lambda cmd: "/usr/bin/clang-format" if cmd == "clang-format" else None)

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
    monkeypatch.setattr(dev, 'REPOSITORY_ROOT', tmp_path)
    monkeypatch.setattr(dev, '_find_clang_format', lambda: "/usr/bin/clang-format")
    monkeypatch.setattr(dev, '_apply_format_chunk', lambda _binary, chunk, _check: format_calls.append(chunk) or 0)

    assert dev.run_format(staged=True) == 1
    assert format_calls == []
    assert "refusing to format partially staged files" in capsys.readouterr().err
    assert subprocess.run(
        ["git", "show", ":Partial.cpp"], cwd=tmp_path, capture_output=True, text=True, check=True
    ).stdout == staged_contents


def test_doctor_runs_gracefully_with_broken_env_file(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    broken_env = write_env(tmp_path / LOCAL_ENV_FILENAME, "INVALID-ENV-LINE\n")
    monkeypatch.setattr(dev, 'DEFAULT_ENV_FILE', broken_env)

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

    monkeypatch.setattr(dev, 'execute_subprocess', fail_ctest)
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


def _gui_coff(names, bigobj=False):
    header = bytearray(56 if bigobj else 20)
    records = bytearray()
    strings = bytearray(b"\0\0\0\0")
    for name, kind in names:
        offset = len(strings)
        strings.extend(name.encode() + b"\0")
        record = bytearray(struct.pack("<II", 0, offset))
        record.extend(struct.pack("<IiHBB" if bigobj else "<IhHBB", 0, 0, kind, 2, 0))
        records.extend(record)
    struct.pack_into("<I", strings, 0, len(strings))
    if bigobj:
        header[:4] = b"\0\0\xff\xff"
    struct.pack_into("<II", header, 48 if bigobj else 8, len(header), len(names))
    return header + records + strings


@pytest.mark.parametrize("bigobj", [False, True])
def test_reads_compiler_external_symbols(tmp_path, bigobj):
    path = tmp_path / "source.obj"
    names = [("?Draw@Editor@Horo@@YAXXZ", 0x20), ("?GImGui@@3PEAUImGuiContext@@EA", 0)]
    path.write_bytes(_gui_coff(names, bigobj))
    assert list(dev._gui_live_symbols(path)) == [(name, kind, True) for name, kind in names]


def test_exports_host_functions_without_marking_executable_as_dll(tmp_path):
    obj = tmp_path / "GUI object.obj"
    clipper_constructor = "??0ImGuiListClipper@@QEAA@XZ"
    clipper_destructor = "??1ImGuiListClipper@@QEAA@XZ"
    obj.write_bytes(_gui_coff([("?Draw@Editor@Horo@@YAXXZ", 0x20), ("?ImHashStr@@YAXXZ", 0x20),
                              (clipper_constructor, 0x20), (clipper_destructor, 0x20), ("__imp_CreateWindow", 0x20)]))
    database = tmp_path / "compile_commands.json"
    database.write_text(json.dumps([{"directory": str(tmp_path), "file": "Gui.cpp", "command": f'cl -DDEBUG /Fo"{obj.name}" HoroGui.dir/Gui.cpp'}]))
    output = tmp_path / "host.def"
    assert dev.main(["gui-live-bridge", "--compile-commands", str(database), "--output", str(output), "--image", "host.exe"]) == 0
    exports = output.read_text()
    assert exports.startswith("EXPORTS\n")
    assert "LIBRARY" not in exports
    assert "?Draw@Editor@Horo@@YAXXZ" in exports
    assert "?ImHashStr@@YAXXZ" in exports
    assert clipper_constructor in exports
    assert clipper_destructor in exports
    assert "?GImGui@@3PEAUImGuiContext@@EA DATA" in exports
    assert "CreateWindow" not in exports
    flags = output.with_suffix(".rsp").read_text()
    assert not any(flag.startswith("/EH") for flag in flags.splitlines())
    assert "/DHORO_GUI_RUNTIME_COMPILATION=1" in flags
    assert "-DDEBUG" in flags


def test_rejects_another_targets_compile_database(tmp_path):
    database = tmp_path / "compile_commands.json"
    database.write_text("[]")
    with pytest.raises(ValueError, match="No HoroGui"):
        dev._generate_gui_live_bridge(database, tmp_path / "host.def", "host.exe")


@pytest.mark.parametrize("use_arguments", [False, True])
def test_mac_bridge_preserves_host_abi_without_reusing_object_or_pch_flags(tmp_path, use_arguments):
    import shlex

    arguments = ["/usr/bin/clang++", "-DHOST_VALUE=with spaces", "-D", 'HOST_QUOTE="quoted"',
                 "-isysroot", "/SDK with spaces", "-arch", "arm64", "-mmacosx-version-min=14.0",
                 "-std=c++20", "-stdlib=libc++", "-include-pch", "/host/pch", "-o", "HoroGui.dir/Gui.cpp.o",
                 "-c", "Gui.cpp"]
    entry = {"directory": str(tmp_path), "file": "Gui.cpp"}
    entry["arguments" if use_arguments else "command"] = arguments if use_arguments else shlex.join(arguments)
    database = tmp_path / "compile_commands.json"
    database.write_text(json.dumps([entry]))
    output = tmp_path / "host flags.rsp"
    assert dev.main(["gui-live-bridge", "--compile-commands", str(database), "--output", str(output),
                     "--image", "HoroEditor", "--platform", "darwin"]) == 0
    flags = shlex.split(output.read_text())
    assert "-DHOST_VALUE=with spaces" in flags
    assert 'HOST_QUOTE="quoted"' in flags
    assert flags[flags.index("-isysroot") + 1] == "/SDK with spaces"
    assert flags[flags.index("-arch") + 1] == "arm64"
    assert "-mmacosx-version-min=14.0" in flags
    assert "-stdlib=libc++" in flags
    assert "-DHORO_GUI_RUNTIME_COMPILATION=1" in flags
    assert "-include-pch" not in flags
    assert "-o" not in flags
    assert not output.with_suffix(".def").exists()
