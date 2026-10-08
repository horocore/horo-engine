#!/usr/bin/env python3
"""Developer runner, formatting, and toolchain health check for Horo Engine."""

from __future__ import annotations

import argparse
from enum import Enum
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Mapping, Sequence
from urllib.error import HTTPError
from urllib.parse import ParseResult, urlparse
from urllib.request import Request, urlopen

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENV_FILE_NAME = ".env.local"
DEFAULT_ENV_FILE = REPOSITORY_ROOT / DEFAULT_ENV_FILE_NAME
DEFAULT_BUILD_DIRECTORY = REPOSITORY_ROOT / "build" / "dev-editor"
DEFAULT_CI_BUILD_DIRECTORY = REPOSITORY_ROOT / "build" / "ci"
DEFAULT_OTLP_ENDPOINT = "http://127.0.0.1:4318"
DEFAULT_GRAFANA_URL = "http://127.0.0.1:3000"
GRAFANA_DASHBOARD_PATH = REPOSITORY_ROOT / "observability" / "grafana" / "dashboards" / "horo-editor.json"
GRAFANA_DASHBOARD_UID = "horo-editor-observability"
GRAFANA_SOURCE_TAG_PREFIX = "horo-source:"
_ENVIRONMENT_KEY = re.compile(r"^[A-Za-z_]\w*$", re.ASCII)

# Diagnostic Item String Constants (Sonar S1192)
_TOOLCHAIN_COMPILER_NAME = "C++ Compiler"
_TOOLCHAIN_CMAKE_NAME = "CMake"
_TOOLCHAIN_NINJA_NAME = "Ninja Generator"
_TOOLCHAIN_CLANG_FORMAT_NAME = "clang-format"
_TOOLCHAIN_CCACHE_NAME = "ccache"
_PYTHON_PYTEST_NAME = "pytest Module"
_GRAPHICS_DISPLAY_NAME = "Interactive Display"
_OBSERVABILITY_OTLP_NAME = "OTLP Collector"
_OBSERVABILITY_GRAFANA_NAME = "Grafana Service"
_SONAR_TOKEN_ENV_KEY = "HORO_SONAR_TOKEN"
_SONAR_DEFAULT_COMPILE_COMMANDS = "build/ci/compile_commands.json"

_CPP_EXTENSIONS = {".h", ".hpp", ".cpp", ".c", ".cc", ".cxx", ".mm", ".m"}
_EXCLUDED_PATH_PARTS = {"vendor", "build", "deprecated", ".horo", ".venv"}


class ConfigurationError(ValueError):
    """Raised when the local developer configuration is invalid."""


class CheckStatus(Enum):
    """Status level for an environment check item."""

    OK = "OK"
    WARN = "WARN"
    ERROR = "ERROR"
    INFO = "INFO"


@dataclass(frozen=True)
class Endpoint:
    """Validated OTLP base endpoint and its TCP reachability address."""

    url: str
    host: str
    port: int


@dataclass(frozen=True)
class DeveloperSettings:
    """Resolved settings consumed only by this developer runner."""

    otel_export_enabled: bool
    endpoint: Endpoint
    grafana_endpoint: Endpoint | None = None
    grafana_api_token: str | None = None


@dataclass
class DoctorItem:
    """Single diagnostic report entry."""

    category: str
    name: str
    status: CheckStatus
    message: str
    details: str | None = None


@dataclass
class DoctorReport:
    """Aggregated doctor diagnostic report."""

    items: list[DoctorItem] = field(default_factory=list)

    def add(
        self,
        category: str,
        name: str,
        status: CheckStatus,
        message: str,
        details: str | None = None,
    ) -> None:
        """Append an item to the report."""
        self.items.append(DoctorItem(category, name, status, message, details))

    @property
    def has_errors(self) -> bool:
        """Return true if any critical check failed."""
        return any(item.status == CheckStatus.ERROR for item in self.items)

    @property
    def has_warnings(self) -> bool:
        """Return true if any warning check occurred."""
        return any(item.status == CheckStatus.WARN for item in self.items)

    def to_dict(self) -> dict[str, object]:
        """Serialize report to a plain dictionary for JSON export."""
        return {
            "has_errors": self.has_errors,
            "has_warnings": self.has_warnings,
            "checks": [
                {
                    "category": item.category,
                    "name": item.name,
                    "status": item.status.value,
                    "message": item.message,
                    "details": item.details,
                }
                for item in self.items
            ],
        }

    def _render_summary(self, use_color: bool) -> str:
        """Format the summary line based on error and warning state."""
        if self.has_errors:
            return (
                "\033[31mStatus: Critical issues found. Please address the errors above.\033[0m"
                if use_color
                else "Status: Critical issues found. Please address the errors above."
            )
        if self.has_warnings:
            return (
                "\033[33mStatus: Ready with warnings.\033[0m"
                if use_color
                else "Status: Ready with warnings."
            )
        return (
            "\033[32mStatus: All development environment checks passed!\033[0m"
            if use_color
            else "Status: All development environment checks passed!"
        )

    def render(self, use_color: bool = True) -> str:
        """Format the report into a clean, human-readable terminal string."""
        encoding = getattr(sys.stdout, "encoding", "") or ""
        supports_unicode = "utf" in encoding.lower()

        ok_sym = "[✓]" if supports_unicode else "[OK]"
        warn_sym = "[!]"
        err_sym = "[✗]" if supports_unicode else "[FAIL]"
        info_sym = "[i]"

        symbols = {
            CheckStatus.OK: (f"\033[32m{ok_sym}\033[0m" if use_color else ok_sym),
            CheckStatus.WARN: (f"\033[33m{warn_sym}\033[0m" if use_color else warn_sym),
            CheckStatus.ERROR: (f"\033[31m{err_sym}\033[0m" if use_color else err_sym),
            CheckStatus.INFO: (f"\033[36m{info_sym}\033[0m" if use_color else info_sym),
        }

        lines: list[str] = ["Horo Developer Doctor Report", "============================", ""]
        current_category: str | None = None

        for item in self.items:
            if item.category != current_category:
                current_category = item.category
                lines.append(f"== {current_category} ==")

            sym = symbols.get(item.status, "[?]")
            line = f" {sym} {item.name}: {item.message}"
            if item.details:
                line += f" ({item.details})"
            lines.append(line)

        lines.append("")
        lines.append(self._render_summary(use_color))
        return "\n".join(lines)


def _parse_dotenv_line(path: Path, line_number: int, line: str) -> tuple[str, str] | None:
    """Parse one single line from a dotenv file."""
    if not line or line.startswith("#"):
        return None
    if line.startswith("export ") or "=" not in line:
        raise ConfigurationError(f"{path}:{line_number}: expected KEY=value")
    key, raw_value = line.split("=", 1)
    if not _ENVIRONMENT_KEY.fullmatch(key):
        raise ConfigurationError(f"{path}:{line_number}: invalid environment key {key!r}")
    value = raw_value.strip()
    if "$(" in value or "${" in value or "`" in value:
        raise ConfigurationError(f"{path}:{line_number}: environment expansion and commands are not supported")
    if value.startswith(("\"", "'")):
        if len(value) < 2 or value[-1] != value[0]:
            raise ConfigurationError(f"{path}:{line_number}: unterminated quoted value")
        return key, value[1:-1]
    if any(character.isspace() for character in value):
        raise ConfigurationError(f"{path}:{line_number}: values containing whitespace must be quoted")
    return key, value


def parse_dotenv(path: Path) -> dict[str, str]:
    """Parse the intentionally small, non-executable developer dotenv grammar."""
    if not path.exists():
        return {}

    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        parsed = _parse_dotenv_line(path, line_number, raw_line.strip())
        if parsed is not None:
            key, value = parsed
            if key in values:
                raise ConfigurationError(f"{path}:{line_number}: duplicate environment key {key!r}")
            values[key] = value
    return values


def parse_enabled(value: str, setting_name: str = "HORO_DEV_OTEL_EXPORT") -> bool:
    """Parse the explicit ON/OFF developer switch."""
    normalized = value.strip().upper()
    if normalized == "ON":
        return True
    if normalized == "OFF":
        return False
    raise ConfigurationError(f"{setting_name} must be ON or OFF")


def _validate_url_structure(parsed: ParseResult, setting_name: str) -> None:
    """Validate protocol and general URL constraints."""
    if parsed.scheme not in {"http", "https"}:
        raise ConfigurationError(f"{setting_name} must use http or https")
    if parsed.username is not None or parsed.password is not None:
        raise ConfigurationError(f"{setting_name} must not include credentials")
    if parsed.path not in {"", "/"} or parsed.params or parsed.query or parsed.fragment:
        raise ConfigurationError(f"{setting_name} must be a base URL without a path, query, or fragment")


def validate_endpoint(value: str) -> Endpoint:
    """Validate an OTLP HTTP base URL without sending protocol traffic."""
    if any(character.isspace() for character in value) or "?" in value or "#" in value:
        raise ConfigurationError("HORO_OTEL_ENDPOINT must not contain whitespace, a query, or a fragment")
    try:
        parsed = urlparse(value)
        port = parsed.port
    except ValueError as error:
        raise ConfigurationError(f"HORO_OTEL_ENDPOINT is invalid: {error}") from error

    _validate_url_structure(parsed, "HORO_OTEL_ENDPOINT")
    if parsed.hostname is None or port is None:
        raise ConfigurationError("HORO_OTEL_ENDPOINT must include a host and explicit port")
    if parsed.scheme == "http" and parsed.hostname not in {"127.0.0.1", "localhost", "::1"}:
        raise ConfigurationError("plaintext HORO_OTEL_ENDPOINT is allowed only for localhost")

    return Endpoint(url=value.rstrip("/"), host=parsed.hostname, port=port)


def validate_grafana_url(value: str) -> Endpoint:
    """Validate a loopback-only Grafana base URL used for automatic imports."""
    if any(character.isspace() for character in value) or "?" in value or "#" in value:
        raise ConfigurationError("HORO_GRAFANA_URL must not contain whitespace, a query, or a fragment")
    try:
        parsed = urlparse(value)
        port = parsed.port
    except ValueError as error:
        raise ConfigurationError(f"HORO_GRAFANA_URL is invalid: {error}") from error

    _validate_url_structure(parsed, "HORO_GRAFANA_URL")
    if parsed.hostname not in {"127.0.0.1", "localhost", "::1"} or port is None:
        raise ConfigurationError("HORO_GRAFANA_URL must use a loopback host and explicit port")
    return Endpoint(url=value.rstrip("/"), host=parsed.hostname, port=port)


def load_settings(path: Path = DEFAULT_ENV_FILE, environ: Mapping[str, str] | None = None) -> DeveloperSettings:
    """Resolve defaults, local configuration, then process-environment overrides."""
    process_environment = os.environ if environ is None else environ
    resolved = {
        "HORO_DEV_OTEL_EXPORT": "OFF",
        "HORO_OTEL_ENDPOINT": DEFAULT_OTLP_ENDPOINT,
        "HORO_DEV_GRAFANA_AUTO_IMPORT": "ON",
        "HORO_GRAFANA_URL": DEFAULT_GRAFANA_URL,
        **parse_dotenv(path),
        **process_environment,
    }
    export_enabled = parse_enabled(resolved["HORO_DEV_OTEL_EXPORT"])
    grafana_auto_import = parse_enabled(resolved["HORO_DEV_GRAFANA_AUTO_IMPORT"], "HORO_DEV_GRAFANA_AUTO_IMPORT")
    grafana_token = resolved.get("HORO_GRAFANA_API_TOKEN")
    return DeveloperSettings(
        otel_export_enabled=export_enabled,
        endpoint=validate_endpoint(resolved["HORO_OTEL_ENDPOINT"] if export_enabled else DEFAULT_OTLP_ENDPOINT),
        grafana_endpoint=validate_grafana_url(resolved["HORO_GRAFANA_URL"]) if grafana_auto_import else None,
        grafana_api_token=grafana_token.strip() if grafana_token else None,
    )


def collector_is_reachable(endpoint: Endpoint, timeout_seconds: float = 0.5) -> bool:
    """Check only whether a TCP listener is reachable at the configured address."""
    try:
        with socket.create_connection((endpoint.host, endpoint.port), timeout=timeout_seconds):
            return True
    except OSError:
        return False


def _grafana_source_tag(path: Path) -> str:
    """Return the stable tag used to avoid importing an unchanged dashboard."""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()[:16]
    return f"{GRAFANA_SOURCE_TAG_PREFIX}{digest}"


def _read_json(request: Request, timeout_seconds: float) -> dict[str, object]:
    """Execute one bounded Grafana API request and decode its JSON response."""
    with urlopen(request, timeout=timeout_seconds) as response:
        return json.loads(response.read().decode("utf-8"))


def _grafana_headers(api_token: str | None = None) -> dict[str, str]:
    """Return standard Grafana request headers including optional bearer token."""
    headers = {"Content-Type": "application/json"}
    if api_token:
        headers["Authorization"] = f"Bearer {api_token}"
    return headers


def sync_grafana_dashboard(
    endpoint: Endpoint,
    dashboard_path: Path = GRAFANA_DASHBOARD_PATH,
    api_token: str | None = None,
) -> str:
    """Import the versioned dashboard when local Grafana is available and stale."""
    headers = _grafana_headers(api_token)
    try:
        _read_json(Request(f"{endpoint.url}/api/health", headers=headers), 0.5)
    except (OSError, ValueError):
        return "unavailable"

    try:
        source_tag = _grafana_source_tag(dashboard_path)
    except OSError as error:
        print(f"warning: Grafana dashboard source could not be read: {error}; continuing editor startup.", file=sys.stderr)
        return "unavailable"
    dashboard_exists = False
    try:
        remote = _read_json(Request(f"{endpoint.url}/api/dashboards/uid/{GRAFANA_DASHBOARD_UID}", headers=headers), 1.0)
        dashboard_exists = True
        remote_dashboard = remote.get("dashboard", {})
        if isinstance(remote_dashboard, dict) and source_tag in remote_dashboard.get("tags", []):
            return "current"
    except HTTPError as error:
        if error.code != 404:
            print(f"warning: Grafana dashboard lookup failed with HTTP {error.code}; continuing without import.", file=sys.stderr)
            return "unavailable"
    except (OSError, ValueError) as error:
        print(f"warning: Grafana dashboard lookup failed: {error}; continuing without import.", file=sys.stderr)
        return "unavailable"

    try:
        dashboard = json.loads(dashboard_path.read_text(encoding="utf-8"))
        tags = [tag for tag in dashboard.get("tags", []) if not tag.startswith(GRAFANA_SOURCE_TAG_PREFIX)]
        dashboard["tags"] = [*tags, source_tag]
        dashboard["id"] = None
        dashboard["version"] = 0
        body = json.dumps(
            {
                "dashboard": dashboard,
                "overwrite": True,
                "message": "Synchronize the repository HoroEditor observability dashboard",
            }
        ).encode("utf-8")
        request = Request(
            f"{endpoint.url}/api/dashboards/db",
            data=body,
            headers=headers,
            method="POST",
        )
        result = _read_json(request, 2.0)
        if result.get("status") != "success":
            raise ValueError("Grafana did not confirm the dashboard import")
        return "updated" if dashboard_exists else "imported"
    except (OSError, ValueError) as error:
        print(f"warning: Grafana dashboard import failed: {error}; continuing editor startup.", file=sys.stderr)
    return "unavailable"


def _compiler_cache_arguments(
    compiler_launcher: str | None,
    msvc_debug_information_format: str | None,
) -> list[str]:
    """Configure both language launchers and cache-compatible MSVC debug output."""
    arguments = []
    if compiler_launcher:
        arguments.extend([
            f"-DCMAKE_C_COMPILER_LAUNCHER={compiler_launcher}",
            f"-DCMAKE_CXX_COMPILER_LAUNCHER={compiler_launcher}",
        ])
    if msvc_debug_information_format:
        arguments.append(f"-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT={msvc_debug_information_format}")
    return arguments


def configure_command(
    settings: DeveloperSettings | None = None,
    build_directory: Path = DEFAULT_BUILD_DIRECTORY,
    build_type: str = "Debug",
    testing: bool = False,
    editor_gui: bool = True,
    render_opengl: bool = True,
    render_metal: bool | None = None,
    telemetry: bool = True,
    opentelemetry: bool | None = None,
    imgui_ui_tests: bool = False,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    extra_cmake_args: Sequence[str] | None = None,
) -> list[str]:
    """Build the typed, parameterizable CMake configure command."""
    if render_metal is None:
        render_metal = sys.platform == "darwin" and editor_gui
    if opentelemetry is None:
        opentelemetry = settings.otel_export_enabled if settings else False

    command = [
        "cmake",
        "-S",
        str(REPOSITORY_ROOT),
        "-B",
        str(build_directory),
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DHORO_BUILD_EDITOR_GUI={'ON' if editor_gui else 'OFF'}",
        f"-DHORO_BUILD_RENDER_OPENGL={'ON' if render_opengl else 'OFF'}",
        f"-DHORO_BUILD_RENDER_METAL={'ON' if render_metal else 'OFF'}",
        f"-DBUILD_TESTING={'ON' if testing else 'OFF'}",
        f"-DHORO_ENABLE_TELEMETRY={'ON' if telemetry else 'OFF'}",
        f"-DHORO_ENABLE_OPENTELEMETRY={'ON' if opentelemetry else 'OFF'}",
        f"-DHORO_ENABLE_IMGUI_UI_TESTS={'ON' if imgui_ui_tests else 'OFF'}",
    ]
    command.extend(_compiler_cache_arguments(compiler_launcher, msvc_debug_information_format))
    if extra_cmake_args:
        command.extend(extra_cmake_args)
    return command


def build_command(
    build_directory: Path = DEFAULT_BUILD_DIRECTORY,
    target: str | None = "HoroEditor",
    parallel: bool = True,
) -> list[str]:
    """Build the project or a specific target incrementally."""
    command = ["cmake", "--build", str(build_directory)]
    if target:
        command.extend(["--target", target])
    if parallel:
        command.append("--parallel")
    return command


def editor_executable(build_directory: Path = DEFAULT_BUILD_DIRECTORY, platform_name: str = sys.platform) -> Path:
    """Return the directly executable editor path for the host platform."""
    if platform_name == "darwin":
        return build_directory / "apps" / "HoroEditor.app" / "Contents" / "MacOS" / "HoroEditor"
    if platform_name == "win32":
        return build_directory / "apps" / "HoroEditor.exe"
    return build_directory / "apps" / "HoroEditor"


def child_environment(settings: DeveloperSettings, environ: Mapping[str, str] | None = None) -> dict[str, str]:
    """Create the editor environment with an explicit export policy."""
    child = dict(os.environ if environ is None else environ)
    child.pop("HORO_OTEL_EXPORT_APPROVED", None)
    child.pop("HORO_OTEL_ENDPOINT", None)
    child.pop("HORO_DEV_OTEL_EXPORT", None)
    if settings.otel_export_enabled:
        child["HORO_OTEL_EXPORT_APPROVED"] = "1"
        child["HORO_OTEL_ENDPOINT"] = settings.endpoint.url
    return child


def print_summary(settings: DeveloperSettings, collector_status: str, build_directory: Path = DEFAULT_BUILD_DIRECTORY) -> None:
    """Print the effective non-secret developer configuration."""
    try:
        displayed_build_directory = build_directory.relative_to(REPOSITORY_ROOT)
    except ValueError:
        displayed_build_directory = build_directory
    print("Horo Developer Runner")
    print()
    print("Target:          HoroEditor")
    print(f"Build directory: {displayed_build_directory}")
    print("Build type:      Debug")
    print(f"OpenTelemetry:   {'enabled' if settings.otel_export_enabled else 'disabled'}")
    if settings.otel_export_enabled:
        print(f"OTLP endpoint:   {settings.endpoint.url}")
        print(f"Collector:       {collector_status}")
    else:
        print("Collector:       not checked")
    print()


def execute_subprocess(command: Sequence[str], cwd: Path = REPOSITORY_ROOT, env: Mapping[str, str] | None = None) -> int:
    """Run a subprocess with clear error reporting and interrupt handling."""
    try:
        # Arguments are passed directly as an argv sequence; no command shell parses caller-provided values.
        result = subprocess.run(command, cwd=cwd, env=env, check=False)  # noqa: S603, S8705
        return result.returncode
    except FileNotFoundError as error:
        print(f"error: command not found: {error.filename or command[0]}", file=sys.stderr)
        return 127
    except OSError as error:
        print(f"error: failed to execute '{command[0]}': {error}", file=sys.stderr)
        return 126
    except KeyboardInterrupt:
        return 130


def run_editor(settings: DeveloperSettings, editor_arguments: Sequence[str], build_directory: Path = DEFAULT_BUILD_DIRECTORY) -> int:
    """Configure, build, and launch HoroEditor without invoking a shell."""
    collector_status = "not checked"
    if settings.otel_export_enabled:
        collector_status = "reachable" if collector_is_reachable(settings.endpoint) else "unreachable"
    print_summary(settings, collector_status, build_directory)

    if settings.otel_export_enabled and collector_status == "unreachable":
        print(
            f"warning: OpenTelemetry endpoint {settings.endpoint.host}:{settings.endpoint.port} is not reachable.\n"
            "The editor will continue with local diagnostics only.",
            file=sys.stderr,
        )

    print("Configuring HoroEditor...", flush=True)
    configure_code = execute_subprocess(configure_command(settings, build_directory=build_directory))
    if configure_code != 0:
        return configure_code

    print("Building HoroEditor...", flush=True)
    build_code = execute_subprocess(build_command(build_directory=build_directory, target="HoroEditor"))
    if build_code != 0:
        return build_code

    if settings.grafana_endpoint is not None:
        grafana_status = sync_grafana_dashboard(settings.grafana_endpoint, api_token=settings.grafana_api_token)
        print(f"Grafana dashboard: {grafana_status}", flush=True)
    else:
        print("Grafana dashboard: disabled", flush=True)

    executable = editor_executable(build_directory)
    if not executable.is_file():
        print(f"error: HoroEditor executable was not produced at {executable}", file=sys.stderr)
        return 1

    print("Launching...", flush=True)
    return execute_subprocess(
        [str(executable), *editor_arguments],
        cwd=REPOSITORY_ROOT,
        env=child_environment(settings),
    )


def run_build(
    target: str | None = None,
    build_directory: Path = DEFAULT_BUILD_DIRECTORY,
    build_type: str = "Debug",
    testing: bool = True,
    imgui_ui_tests: bool = False,
    clean: bool = False,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    extra_cmake_args: Sequence[str] | None = None,
    fetchcontent_base_dir: Path | None = None,
) -> int:
    """Configure and build the repository or a specific target."""
    if clean and build_directory.exists():
        print(f"Cleaning build directory {build_directory}...", flush=True)
        shutil.rmtree(build_directory, ignore_errors=True)

    print(f"Configuring project in {build_directory}...", flush=True)
    cmake_arguments = list(extra_cmake_args or ())
    if fetchcontent_base_dir is not None:
        cmake_arguments.insert(0, f"-DFETCHCONTENT_BASE_DIR={fetchcontent_base_dir.resolve()}")
    cfg_cmd = configure_command(
        build_directory=build_directory,
        build_type=build_type,
        testing=testing,
        editor_gui=True,
        render_opengl=True,
        render_metal=(sys.platform == "darwin"),
        telemetry=True,
        opentelemetry=True,
        imgui_ui_tests=imgui_ui_tests,
        compiler_launcher=compiler_launcher,
        msvc_debug_information_format=msvc_debug_information_format,
        extra_cmake_args=cmake_arguments,
    )
    configure_code = execute_subprocess(cfg_cmd)
    if configure_code != 0:
        return configure_code

    print(f"Building {'target ' + target if target else 'all targets'}...", flush=True)
    return execute_subprocess(build_command(build_directory=build_directory, target=target))


def run_tests(
    regex: str | None = None,
    exclude: str | None = None,
    gui: bool = False,
    build_directory: Path = DEFAULT_CI_BUILD_DIRECTORY,
    build_type: str = "Debug",
    junit: Path | None = None,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    extra_ctest_args: Sequence[str] | None = None,
    fetchcontent_base_dir: Path | None = None,
    skip_build: bool = False,
) -> int:
    """Build unless explicitly skipped, then execute CTest with optional filters."""
    if skip_build:
        if not (build_directory / "CTestTestfile.cmake").is_file():
            print(f"error: --skip-build requires a configured test directory: {build_directory}", file=sys.stderr)
            return 2
    else:
        build_code = run_build(
            target=None,
            build_directory=build_directory,
            build_type=build_type,
            testing=True,
            imgui_ui_tests=gui,
            compiler_launcher=compiler_launcher,
            msvc_debug_information_format=msvc_debug_information_format,
            fetchcontent_base_dir=fetchcontent_base_dir,
        )
        if build_code != 0:
            return build_code

    ctest_command = ["ctest", "--test-dir", str(build_directory), "--output-on-failure"]
    if skip_build:
        ctest_command.append("--no-tests=error")
    if not gui:
        ctest_command.extend(["-LE", "gui"])
    if regex:
        ctest_command.extend(["-R", regex])
    if exclude:
        ctest_command.extend(["-E", exclude])
    if junit:
        junit.parent.mkdir(parents=True, exist_ok=True)
        ctest_command.extend(["--output-junit", str(junit)])
    if extra_ctest_args:
        ctest_command.extend(extra_ctest_args)

    print(f"Running CTest suite in {build_directory}...", flush=True)
    return execute_subprocess(ctest_command)


def run_check(
    regex: str | None = None,
    gui: bool = False,
    build_directory: Path = DEFAULT_CI_BUILD_DIRECTORY,
    junit: Path | None = None,
    compiler_launcher: str | None = None,
    msvc_debug_information_format: str | None = None,
    fetchcontent_base_dir: Path | None = None,
) -> int:
    """Run full CI-parity check: configure all components, compile all targets, run test suite."""
    print("==================================================", flush=True)
    print("  Horo Engine CI Parity Verification Pass", flush=True)
    print("==================================================", flush=True)
    return run_tests(
        regex=regex,
        gui=gui,
        build_directory=build_directory,
        junit=junit,
        compiler_launcher=compiler_launcher,
        msvc_debug_information_format=msvc_debug_information_format,
        fetchcontent_base_dir=fetchcontent_base_dir,
    )


def _is_formattable(path: Path) -> bool:
    """Return true if path is a repository C++ source and not third-party."""
    try:
        relative = path.relative_to(REPOSITORY_ROOT) if path.is_absolute() else path
    except ValueError:
        return False
    return relative.suffix in _CPP_EXTENSIONS and not any(part in _EXCLUDED_PATH_PARTS for part in relative.parts)


def _collect_format_candidates(files: Sequence[str] | None, staged: bool) -> list[Path]:
    """Resolve file paths to format based on staged flag or explicit arguments."""
    if files:
        return [p for f in files if (p := Path(f).resolve()).is_file() and p.suffix in _CPP_EXTENSIONS]

    if staged:
        proc = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        candidates = [REPOSITORY_ROOT / line.strip() for line in proc.stdout.splitlines() if line.strip()]
    else:
        proc = subprocess.run(["git", "ls-files"], cwd=REPOSITORY_ROOT, capture_output=True, text=True, check=False)
        candidates = [REPOSITORY_ROOT / line.strip() for line in proc.stdout.splitlines() if line.strip()]

    return [p for p in candidates if _is_formattable(p) and p.is_file()]


def _find_clang_format() -> str | None:
    """Resolve clang-format binary, preferring native executable over batch/cmd wrappers on Windows."""
    if sys.platform == "win32":
        exe = shutil.which("clang-format.exe")
        if exe:
            return exe
    return shutil.which("clang-format")


def _apply_format_chunk(clang_format_bin: str, chunk: list[str], check: bool) -> int:
    """Apply or verify clang-format on one chunk of files. Returns 0 on success."""
    if check:
        result = execute_subprocess([clang_format_bin, "--dry-run", "--Werror", *chunk])
        if result != 0:
            print("Formatting violations detected in above files.", file=sys.stderr)
            return 1
        return 0
    return execute_subprocess([clang_format_bin, "-i", *chunk])


def _partially_staged_files(paths: Sequence[Path]) -> list[Path]:
    """Return staged targets whose working-tree contents differ from the index."""
    relative_paths = [os.path.relpath(path, REPOSITORY_ROOT) for path in paths]
    proc = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", "--", *relative_paths],
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or "git diff failed"
        raise RuntimeError(f"Could not inspect unstaged changes: {detail}")
    return [REPOSITORY_ROOT / line for line in proc.stdout.splitlines() if line]


def _validate_staged_format(target_files: Sequence[Path]) -> bool:
    try:
        partially_staged = _partially_staged_files(target_files)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return False
    if partially_staged:
        print(
            "error: refusing to format partially staged files; stage or stash their unstaged changes first:",
            file=sys.stderr,
        )
        for path in partially_staged:
            print(f"  {os.path.relpath(path, REPOSITORY_ROOT)}", file=sys.stderr)
        return False
    return True


def run_format(files: Sequence[str] | None = None, staged: bool = False, check: bool = False) -> int:
    """Format C++ source files with clang-format, with support for staged commits and dry-run check."""
    clang_format_bin = _find_clang_format()
    if not clang_format_bin:
        print("error: clang-format not found in PATH", file=sys.stderr)
        return 127

    target_files = _collect_format_candidates(files, staged)
    if not target_files:
        print("No matching C++ files to format.")
        return 0

    if staged and not check and not _validate_staged_format(target_files):
        return 1

    file_paths = [str(f) for f in target_files]
    chunk_size = 50
    chunks = [file_paths[i:i + chunk_size] for i in range(0, len(file_paths), chunk_size)]

    for chunk in chunks:
        code = _apply_format_chunk(clang_format_bin, chunk, check)
        if code != 0:
            return code

    if staged and not check:
        for chunk in chunks:
            subprocess.run(["git", "add", *chunk], cwd=REPOSITORY_ROOT, check=False)  # noqa: S603

    if check:
        print(f"All {len(target_files)} file(s) correctly formatted.")
    else:
        print(f"Successfully formatted {len(target_files)} file(s).")
    return 0



def _check_tool_version(
    report: DoctorReport,
    category: str,
    name: str,
    binary_candidates: Sequence[str | None],
    version_flag: str = "--version",
    missing_status: CheckStatus = CheckStatus.ERROR,
    missing_message: str | None = None,
) -> str | None:
    """Find a candidate binary, execute its version flag, and record diagnostic output."""
    found_bin: str | None = None
    for cand in binary_candidates:
        if cand and shutil.which(cand):
            found_bin = cand
            break

    if not found_bin:
        msg = missing_message or f"{name} not found in PATH"
        report.add(category, name, missing_status, msg)
        return None

    try:
        kwargs = {}
        if sys.platform == "win32" and found_bin.lower().endswith((".cmd", ".bat")):
            kwargs["shell"] = True
        proc = subprocess.run([found_bin, version_flag], capture_output=True, text=True, check=False, **kwargs)
        first_line = proc.stdout.splitlines()[0] if proc.stdout else "available"
        report.add(category, name, CheckStatus.OK, first_line, details=f"binary: {found_bin}")
        return found_bin
    except OSError as error:
        report.add(category, name, CheckStatus.ERROR, f"Failed to execute {name}: {error}")
        return None


def check_compiler(report: DoctorReport) -> None:
    """Verify C++ compiler availability and C++20 support."""
    cxx_env = os.environ.get("CXX")
    candidates = [cxx_env] if cxx_env else ["clang++", "g++", "cl.exe", "c++"]
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_COMPILER_NAME,
        candidates,
        missing_message="No C++ compiler found in PATH or $CXX",
    )


def check_cmake(report: DoctorReport) -> None:
    """Verify CMake and Ninja build generator availability."""
    _check_tool_version(report, "Toolchain", _TOOLCHAIN_CMAKE_NAME, ["cmake"])
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_NINJA_NAME,
        ["ninja"],
        missing_status=CheckStatus.WARN,
        missing_message="ninja not found; CMake may fall back to make/msbuild",
    )


def check_format_tool(report: DoctorReport) -> None:
    """Check if clang-format is available for code formatting."""
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_CLANG_FORMAT_NAME,
        ["clang-format.exe", "clang-format"],
        missing_status=CheckStatus.WARN,
        missing_message="clang-format not found; pre-commit format check will be skipped",
    )


def check_ccache(report: DoctorReport) -> None:
    """Check if ccache is available for build acceleration."""
    _check_tool_version(
        report,
        "Toolchain",
        _TOOLCHAIN_CCACHE_NAME,
        ["ccache"],
        missing_status=CheckStatus.WARN,
        missing_message="ccache not found; installing ccache will significantly speed up rebuilds",
    )


def check_python_environment(report: DoctorReport) -> None:
    """Check Python version and test runners (pytest / .venv)."""
    py_ver = f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    report.add("Python", "Interpreter", CheckStatus.OK, f"Python {py_ver} ({sys.executable})")

    venv_path = REPOSITORY_ROOT / ".venv"
    if venv_path.is_dir():
        report.add("Python", "Virtual Environment", CheckStatus.OK, f".venv present at {venv_path}")
    else:
        report.add("Python", "Virtual Environment", CheckStatus.INFO, ".venv not found in repository root")

    pytest_found = False
    try:
        import pytest  # noqa: F401
        report.add("Python", _PYTHON_PYTEST_NAME, CheckStatus.OK, f"pytest {pytest.__version__} installed")
        pytest_found = True
    except ImportError:
        pass

    if not pytest_found and venv_path.is_dir():
        venv_pytest = venv_path / "bin" / "pytest"
        if venv_pytest.is_file():
            report.add("Python", _PYTHON_PYTEST_NAME, CheckStatus.OK, f"pytest available via .venv ({venv_pytest})")
            pytest_found = True

    if not pytest_found:
        report.add("Python", _PYTHON_PYTEST_NAME, CheckStatus.WARN, "pytest not found; python test suite will be skipped in CTest")


def _check_darwin_graphics(report: DoctorReport) -> None:
    """Check macOS Metal, OpenGL, and window server status."""
    sdk_path = "/System/Library/Frameworks"
    metal_present = Path(f"{sdk_path}/Metal.framework").exists() or Path("/Applications/Xcode.app").exists()
    if metal_present:
        report.add("Graphics", "Metal API", CheckStatus.OK, "Apple Metal framework available")
    else:
        report.add("Graphics", "Metal API", CheckStatus.WARN, "Metal framework not found in standard system path")

    opengl_present = Path(f"{sdk_path}/OpenGL.framework").exists()
    if opengl_present:
        report.add("Graphics", "OpenGL API", CheckStatus.OK, "Apple OpenGL framework available")
    else:
        report.add("Graphics", "OpenGL API", CheckStatus.INFO, "OpenGL framework legacy path")

    has_pgrep = shutil.which("pgrep")
    window_server_active = False
    if has_pgrep:
        ws_proc = subprocess.run(["pgrep", "-x", "WindowServer"], capture_output=True, check=False)
        window_server_active = (ws_proc.returncode == 0)

    is_remote_ssh = "SSH_CLIENT" in os.environ or "SSH_CONNECTION" in os.environ
    if window_server_active and not is_remote_ssh:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.OK, "Interactive WindowServer session active")
    else:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.INFO, "Headless or remote session")


def _check_linux_graphics(report: DoctorReport) -> None:
    """Check Linux display and graphics environment."""
    display = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    if display:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.OK, f"Display server active ({display})")
    else:
        report.add("Graphics", _GRAPHICS_DISPLAY_NAME, CheckStatus.INFO, "Headless display environment (DISPLAY not set)")


def check_platform_and_graphics(report: DoctorReport) -> None:
    """Verify host graphics and display environment capabilities."""
    current_os = platform.system()
    arch = platform.machine()
    report.add("Platform", "Host System", CheckStatus.OK, f"{current_os} {platform.release()} ({arch})")

    if current_os == "Darwin":
        _check_darwin_graphics(report)
    elif current_os == "Linux":
        _check_linux_graphics(report)


def check_observability_and_services(report: DoctorReport, settings: DeveloperSettings | None) -> None:
    """Check validity of developer settings and service reachability."""
    env_file = DEFAULT_ENV_FILE
    if env_file.exists():
        try:
            parse_dotenv(env_file)
            report.add("Configuration", DEFAULT_ENV_FILE_NAME, CheckStatus.OK, "Valid configuration file parsed")
        except ConfigurationError as error:
            report.add("Configuration", DEFAULT_ENV_FILE_NAME, CheckStatus.ERROR, f"Syntax/validation error: {error}")
    else:
        report.add("Configuration", DEFAULT_ENV_FILE_NAME, CheckStatus.INFO, f"No local {DEFAULT_ENV_FILE_NAME} file (using defaults)")

    if settings is None:
        report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.WARN, "Settings could not be loaded due to configuration error")
        report.add("Observability", _OBSERVABILITY_GRAFANA_NAME, CheckStatus.WARN, "Settings could not be loaded due to configuration error")
        return

    if settings.otel_export_enabled:
        reachable = collector_is_reachable(settings.endpoint)
        if reachable:
            report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.OK, f"Reachable at {settings.endpoint.url}")
        else:
            report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.WARN, f"Unreachable at {settings.endpoint.url} (fallback to local logs)")
    else:
        report.add("Observability", _OBSERVABILITY_OTLP_NAME, CheckStatus.INFO, "OpenTelemetry export disabled (HORO_DEV_OTEL_EXPORT=OFF)")

    if settings.grafana_endpoint:
        headers = _grafana_headers(settings.grafana_api_token)
        try:
            with urlopen(Request(f"{settings.grafana_endpoint.url}/api/health", headers=headers), timeout=0.5):
                report.add("Observability", _OBSERVABILITY_GRAFANA_NAME, CheckStatus.OK, f"Reachable at {settings.grafana_endpoint.url}")
        except (OSError, ValueError):
            report.add("Observability", _OBSERVABILITY_GRAFANA_NAME, CheckStatus.WARN, f"Unreachable at {settings.grafana_endpoint.url}")
    else:
        report.add("Observability", _OBSERVABILITY_GRAFANA_NAME, CheckStatus.INFO, "Grafana auto-import disabled")



def _find_sonar_scanner() -> Path | None:
    """Locate the SonarScanner executable under tools/sonar-scanner-*/bin/ or in system PATH."""
    tools_dir = REPOSITORY_ROOT / "tools"
    if tools_dir.is_dir():
        for entry in sorted(tools_dir.iterdir(), reverse=True):
            if not entry.is_dir() or not entry.name.startswith("sonar-scanner"):
                continue
            bin_dir = entry / "bin"
            for candidate in ("sonar-scanner.bat", "sonar-scanner.cmd", "sonar-scanner"):
                exe = bin_dir / candidate
                if exe.is_file():
                    return exe
    if system_scanner := shutil.which("sonar-scanner"):
        return Path(system_scanner)
    return None



def _persist_sonar_token(token: str) -> None:
    """Append or update the sonar token in the repository-owned local environment file."""
    key = _SONAR_TOKEN_ENV_KEY
    if DEFAULT_ENV_FILE.exists():
        lines = DEFAULT_ENV_FILE.read_text(encoding="utf-8").splitlines(keepends=True)
        new_lines: list[str] = []
        replaced = False
        for line in lines:
            if line.startswith((f"{key}=", f"{key} =")):  # noqa: S8513
                new_lines.append(f"{key}={token}\n")
                replaced = True
            else:
                new_lines.append(line)
        if not replaced:
            if new_lines and not new_lines[-1].endswith("\n"):
                new_lines.append("\n")
            new_lines.append(f"{key}={token}\n")
        DEFAULT_ENV_FILE.write_text("".join(new_lines), encoding="utf-8")  # NOSONAR(S2083, S8707)
    else:
        DEFAULT_ENV_FILE.write_text(f"{key}={token}\n", encoding="utf-8")  # NOSONAR(S8707)


def _resolve_sonar_token(cli_token: str | None, env_file: Path = DEFAULT_ENV_FILE) -> str | None:
    """Resolve the SonarCloud token using the priority chain: CLI arg → env/dotenv → interactive prompt.

    When the token is obtained interactively, the user is offered the option to
    persist it to the local .env file so it is available in future runs.
    """
    # 1. Explicit CLI argument takes priority.
    if cli_token:
        return cli_token.strip()

    # 2. Environment variable / .env.local file.
    dotenv = parse_dotenv(env_file)
    from_env = os.environ.get(_SONAR_TOKEN_ENV_KEY) or dotenv.get(_SONAR_TOKEN_ENV_KEY)
    if from_env and from_env.strip():
        return from_env.strip()

    # 3. Interactive prompt — only if stdin is a TTY.
    if not sys.stdin.isatty():
        print(
            f"error: SonarCloud token not found. Set {_SONAR_TOKEN_ENV_KEY} in the environment, "
            f"{DEFAULT_ENV_FILE_NAME}, or pass --token.",
            file=sys.stderr,
        )
        return None

    print(f"SonarCloud token not found in environment or {DEFAULT_ENV_FILE_NAME}.")
    print(f"Set {_SONAR_TOKEN_ENV_KEY} in {DEFAULT_ENV_FILE_NAME} to skip this prompt in future runs.")
    try:
        token = input("Enter SonarCloud token: ").strip()
    except (EOFError, KeyboardInterrupt):
        print("", file=sys.stderr)
        return None

    if not token:
        print("error: empty token provided.", file=sys.stderr)
        return None

    # Offer to persist.
    try:
        answer = input(f"Save token to {DEFAULT_ENV_FILE_NAME} for future runs? [y/N] ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        answer = ""
    if answer in {"y", "yes"}:
        try:
            _persist_sonar_token(token)
            print(f"Token saved to {DEFAULT_ENV_FILE_NAME}.")
        except OSError as error:
            print(f"warning: could not persist token: {error}", file=sys.stderr)

    return token


def run_sonar(
    token: str | None = None,
    pr_key: str | None = None,
    compile_commands: Path | None = None,
    env_file: Path = DEFAULT_ENV_FILE,
    extra_sonar_args: Sequence[str] | None = None,
) -> int:
    """Run the local SonarScanner against the current working tree and upload results to SonarCloud."""
    # Resolve token.
    resolved_token = _resolve_sonar_token(token, env_file)
    if not resolved_token:
        return 2

    # Locate scanner executable.
    scanner = _find_sonar_scanner()
    if scanner is None:
        print(
            "error: SonarScanner not found under tools/sonar-scanner-*/bin/.\n"
            "       Download from https://docs.sonarsource.com/sonarcloud/advanced-setup/ci-based-analysis/sonarscanner-cli/",
            file=sys.stderr,
        )
        return 127

    # Resolve compile-commands path.
    cc_path = compile_commands if compile_commands is not None else REPOSITORY_ROOT / _SONAR_DEFAULT_COMPILE_COMMANDS
    if not cc_path.is_file():
        print(
            f"error: compile_commands.json not found at {cc_path}.\n"
            "       Run 'python scripts/dev.py test' first to build the CI configuration.",
            file=sys.stderr,
        )
        return 1

    command: list[str] = [
        str(scanner),
        f"-Dsonar.token={resolved_token}",
        f"-Dsonar.cfamily.compile-commands={cc_path}",
    ]
    if pr_key:
        command.append(f"-Dsonar.pullrequest.key={pr_key}")
    if extra_sonar_args:
        command.extend(extra_sonar_args)

    print("Running SonarScanner...", flush=True)
    if pr_key:
        print(f"  Pull request: {pr_key}", flush=True)
    print(f"  Compile commands: {cc_path}", flush=True)
    print(f"  Scanner: {scanner}", flush=True)
    print()

    return execute_subprocess(command)


def run_doctor(settings: DeveloperSettings | None = None) -> DoctorReport:
    """Execute all diagnostic checks and assemble the report."""
    if settings is None:
        try:
            settings = load_settings()
        except (ConfigurationError, OSError):
            settings = None

    report = DoctorReport()
    check_compiler(report)
    check_cmake(report)
    check_format_tool(report)
    check_ccache(report)
    check_python_environment(report)
    check_platform_and_graphics(report)
    check_observability_and_services(report, settings)
    return report


_EPILOG_EXAMPLES = """examples:
  python3 scripts/dev.py doctor             Diagnose development toolchain and services
  python3 scripts/dev.py test               Run unit and integration test suite
  python3 scripts/dev.py test Scene         Run tests matching 'Scene'
  python3 scripts/dev.py check              Run full CI-parity verification pass
  python3 scripts/dev.py format --staged    Format git-staged C++ source files
  python3 scripts/dev.py build HoroEditor   Build specific target
  python3 scripts/dev.py sonar              Run local SonarScanner and upload to SonarCloud
  python3 scripts/dev.py sonar --pr 47      Run scan against pull request #47
  python3 scripts/dev.py run editor         Configure, build, and launch HoroEditor
  python3 scripts/dev.py help <command>     Show detailed options for a subcommand
"""


def _add_compiler_cache_arguments(parser: argparse.ArgumentParser) -> None:
    """Add dependency-directory and compiler-cache options shared by build commands."""
    parser.add_argument(
        "--fetchcontent-base-dir",
        type=Path,
        default=None,
        help="directory for CMake FetchContent dependencies",
    )
    parser.add_argument(
        "--compiler-launcher",
        default=None,
        help="CMake compiler launcher executable (for example sccache)",
    )
    parser.add_argument(
        "--msvc-debug-information-format",
        choices=("Embedded", "ProgramDatabase", "EditAndContinue"),
        default=None,
        help="select the MSVC debug information representation",
    )


def create_argument_parser() -> argparse.ArgumentParser:
    """Create the developer command-line interface argument parser."""
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=_EPILOG_EXAMPLES,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    commands = parser.add_subparsers(dest="command", required=False)

    # run command
    run = commands.add_parser("run", help="configure, build, and run a supported target")
    run.add_argument("target_or_args", nargs="*", default=[], help="target (default 'editor') or arguments forwarded")

    # doctor command
    doc = commands.add_parser("doctor", help="diagnose toolchain, graphics, and environment health")
    doc.add_argument("--json", action="store_true", help="output report as structured JSON")
    doc.add_argument("--no-color", action="store_true", help="disable ANSI color formatting")

    # build command
    bld = commands.add_parser("build", help="configure and build all targets or a specific target")
    bld.add_argument("target", nargs="?", default=None, help="specific target name to build (e.g. HoroEditor, HoroEditorUiAutomationTests)")
    bld.add_argument("-B", "--dir", type=Path, default=DEFAULT_BUILD_DIRECTORY, help="build output directory")
    bld.add_argument("--type", default="Debug", choices=("Debug", "Release", "RelWithDebInfo"), help="CMake build type")
    bld.add_argument("--clean", action="store_true", help="clean build directory before configuring")
    bld.add_argument("--no-testing", action="store_true", help="disable building tests")
    _add_compiler_cache_arguments(bld)

    # test command
    tst = commands.add_parser("test", help="configure with tests, compile, and run CTest suite")
    tst.add_argument("regex", nargs="?", default=None, help="test name filter regex (passed to ctest -R)")
    tst.add_argument("-R", "--regex-flag", dest="regex_opt", default=None, help="explicit regex filter option")
    tst.add_argument("-E", "--exclude", default=None, help="exclude tests matching regex")
    tst.add_argument("--gui", action="store_true", help="include interactive GUI test engine scenarios")
    tst.add_argument("-B", "--dir", type=Path, default=DEFAULT_CI_BUILD_DIRECTORY, help="test build directory")
    tst.add_argument("--type", default="Debug", choices=("Debug", "Release", "RelWithDebInfo"), help="CMake build type")
    tst.add_argument("--junit", type=Path, default=None, help="write CTest results to JUnit XML file")
    tst.add_argument("--skip-build", action="store_true", help="run CTest against an already built test directory")
    _add_compiler_cache_arguments(tst)

    # check command (CI Parity)
    chk = commands.add_parser("check", help="run full CI-parity build and comprehensive test pass")
    chk.add_argument("-R", "--regex", default=None, help="filter tests during check")
    chk.add_argument("--gui", action="store_true", help="include interactive GUI scenarios")
    chk.add_argument("-B", "--dir", type=Path, default=DEFAULT_CI_BUILD_DIRECTORY, help="check build directory")
    chk.add_argument("--junit", type=Path, default=None, help="write CTest results to JUnit XML file")
    _add_compiler_cache_arguments(chk)

    # format command
    fmt = commands.add_parser("format", help="format C++ source files using clang-format")
    fmt.add_argument("files", nargs="*", default=None, help="specific files to format")
    fmt.add_argument("--staged", action="store_true", help="format only git-staged C++ files and re-add them")
    fmt.add_argument("--check", action="store_true", help="check formatting without modifying files")

    # help command
    hlp = commands.add_parser("help", help="show general help or help for a specific command")
    hlp.add_argument("topic", nargs="?", default=None, help="subcommand name to show detailed help for")

    # sonar command
    snr = commands.add_parser(
        "sonar",
        help="run local SonarScanner and upload analysis to SonarCloud",
        description=(
            "Run the bundled SonarScanner CLI against the repository and upload the analysis report to SonarCloud.\n\n"
            "Token resolution order:\n"
            "  1. --token argument\n"
            f"  2. {_SONAR_TOKEN_ENV_KEY} environment variable\n"
            f"  3. {_SONAR_TOKEN_ENV_KEY} entry in {DEFAULT_ENV_FILE_NAME}\n"
            "  4. Interactive prompt (TTY only) with optional save to .env.local"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    snr.add_argument(
        "--token",
        default=None,
        metavar="TOKEN",
        help=f"SonarCloud authentication token (overrides {_SONAR_TOKEN_ENV_KEY} env var / .env.local)",
    )
    snr.add_argument(
        "--pr",
        "--pullrequest",
        dest="pr_key",
        default=None,
        metavar="PR_NUMBER",
        help="pull request key to analyze (e.g. 47)",
    )
    snr.add_argument(
        "--compile-commands",
        type=Path,
        default=None,
        metavar="PATH",
        help=f"path to compile_commands.json (default: {_SONAR_DEFAULT_COMPILE_COMMANDS})",
    )

    return parser


def _handle_sonar(parsed: argparse.Namespace, extra_args: Sequence[str]) -> int:
    """Handle sonar subcommand."""
    return run_sonar(
        token=parsed.token,
        pr_key=parsed.pr_key,
        compile_commands=parsed.compile_commands,
        extra_sonar_args=list(extra_args) if extra_args else None,
    )


def _handle_run(parsed: argparse.Namespace, settings: DeveloperSettings, extra_args: Sequence[str] | None = None) -> int:
    """Handle run subcommand."""
    raw_args = list(parsed.target_or_args)
    if extra_args:
        raw_args.extend(extra_args)
    if raw_args and raw_args[0] == "editor":
        raw_args.pop(0)
    if raw_args and raw_args[0] == "--":
        raw_args.pop(0)
    try:
        return run_editor(settings, raw_args)
    except KeyboardInterrupt:
        return 130


def _handle_doctor(parsed: argparse.Namespace, settings: DeveloperSettings | None) -> int:
    """Handle doctor subcommand."""
    report = run_doctor(settings)
    if getattr(parsed, "json", False):
        print(json.dumps(report.to_dict(), indent=2))
    else:
        use_color = sys.stdout.isatty() and not getattr(parsed, "no_color", False)
        print(report.render(use_color=use_color))
    return 1 if report.has_errors else 0


def _handle_help(parser: argparse.ArgumentParser, topic: str | None) -> int:
    """Show top-level help or topic-specific subcommand help."""
    if not topic:
        parser.print_help()
        return 0

    # Note: accessing _actions is an internal argparse hook used to display specific subparser help
    subparser_action = next(
        (action for action in parser._actions if isinstance(action, argparse._SubParsersAction)),
        None,
    )
    if subparser_action and topic in subparser_action.choices:
        subparser_action.choices[topic].print_help()
        return 0

    print(f"error: unknown command {topic!r}. Run 'dev.py help' for available commands.", file=sys.stderr)
    return 2


def _configure_terminal_environment() -> None:
    """Enable ANSI/UTF-8 mode on Windows consoles and configure robust encoding fallbacks."""
    if sys.platform == "win32":
        try:
            os.system("")  # Enable VT100 ANSI processing on Windows 10+
        except OSError:
            pass
    if sys.version_info >= (3, 7):
        try:
            if hasattr(sys.stdout, "reconfigure"):
                sys.stdout.reconfigure(errors="replace")
            if hasattr(sys.stderr, "reconfigure"):
                sys.stderr.reconfigure(errors="replace")
        except (AttributeError, OSError, ValueError):
            pass


def _require_no_unparsed(parser: argparse.ArgumentParser, unparsed: Sequence[str]) -> None:
    """Ensure no extra positional or unknown option arguments were provided."""
    if unparsed:
        parser.error(f"unrecognized arguments: {' '.join(unparsed)}")


def _dispatch_command(parsed: argparse.Namespace, unparsed: Sequence[str], parser: argparse.ArgumentParser) -> int:
    """Dispatch parsed subcommand to its respective handler."""
    if not parsed.command or parsed.command == "help":
        return _handle_help(parser, getattr(parsed, "topic", None))

    if parsed.command == "doctor":
        try:
            doc_settings = load_settings()
        except (ConfigurationError, OSError):
            doc_settings = None
        return _handle_doctor(parsed, doc_settings)

    if parsed.command == "format":
        _require_no_unparsed(parser, unparsed)
        return run_format(files=parsed.files, staged=parsed.staged, check=parsed.check)

    if parsed.command == "sonar":
        return _handle_sonar(parsed, unparsed)

    if parsed.command == "build":
        _require_no_unparsed(parser, unparsed)
        return run_build(
            target=parsed.target,
            build_directory=parsed.dir,
            build_type=parsed.type,
            testing=not parsed.no_testing,
            clean=parsed.clean,
            compiler_launcher=parsed.compiler_launcher,
            msvc_debug_information_format=parsed.msvc_debug_information_format,
            fetchcontent_base_dir=parsed.fetchcontent_base_dir,
        )

    if parsed.command == "test":
        _require_no_unparsed(parser, unparsed)
        active_regex = parsed.regex_opt or parsed.regex
        return run_tests(
            regex=active_regex,
            exclude=parsed.exclude,
            gui=parsed.gui,
            build_directory=parsed.dir,
            build_type=parsed.type,
            junit=parsed.junit,
            compiler_launcher=parsed.compiler_launcher,
            msvc_debug_information_format=parsed.msvc_debug_information_format,
            fetchcontent_base_dir=parsed.fetchcontent_base_dir,
            skip_build=parsed.skip_build,
        )

    if parsed.command == "check":
        _require_no_unparsed(parser, unparsed)
        return run_check(
            regex=parsed.regex,
            gui=parsed.gui,
            build_directory=parsed.dir,
            junit=parsed.junit,
            compiler_launcher=parsed.compiler_launcher,
            msvc_debug_information_format=parsed.msvc_debug_information_format,
            fetchcontent_base_dir=parsed.fetchcontent_base_dir,
        )

    # run command
    try:
        settings = load_settings()
    except (ConfigurationError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    return _handle_run(parsed, settings, unparsed)


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the developer command-line interface."""
    _configure_terminal_environment()
    parser = create_argument_parser()
    parsed, unparsed = parser.parse_known_args(arguments)
    return _dispatch_command(parsed, unparsed, parser)


if __name__ == "__main__":
    raise SystemExit(main())
