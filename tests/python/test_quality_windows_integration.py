"""Portable CLI, compiler-context and native analyzer regression cases."""
from __future__ import annotations

import analyze

import json
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import analyze as preflight
import analyze as tools
import analyze as sonar
from analyze import AnalysisError as AnalysisError


def test_msvc_compiler_entry_uses_explicit_output_and_include_paths(tmp_path):
    build = tmp_path / 'build with boşluk'
    build.mkdir()
    source = tmp_path / 'source.cpp'
    source.write_text('int value;\n')
    include = tmp_path / 'include with space'
    database = build / 'compile_commands.json'
    database.write_text(json.dumps([{
        'directory': str(build), 'file': str(source),
        'arguments': ['cl.exe', '/nologo', '/I' + str(include), '/c', str(source)],
        'output': 'CMakeFiles/unit.dir/source.cpp.obj',
    }]))
    entry = sonar.compiler_entries(database, tmp_path)[0]
    assert entry['source'] == source
    assert entry['target'] == 'CMakeFiles/unit.dir/source.cpp.obj'
    assert include in entry['includes']


@pytest.mark.parametrize('prefix', ['/Fo', '-o'])
def test_compiler_entry_reads_attached_object_output(tmp_path, prefix):
    build = tmp_path / 'build'
    build.mkdir()
    source = tmp_path / 'source.cpp'
    source.write_text('int value;\n')
    database = build / 'compile_commands.json'
    database.write_text(json.dumps([{
        'directory': str(build), 'file': str(source),
        'arguments': ['compiler', prefix + 'source.cpp.obj', '/I', str(tmp_path)],
    }]))
    entry = sonar.compiler_entries(database, tmp_path)[0]
    assert entry['target'] == 'source.cpp.obj'
    assert tmp_path in entry['includes']


@pytest.mark.skipif(sys.platform != 'win32', reason='Native Windows command-line grammar')
@pytest.mark.parametrize('build_name', ['build ü', 'build'])
def test_msvc_command_string_preserves_backslashes_and_quoted_includes(tmp_path, build_name):
    build = tmp_path / build_name
    build.mkdir()
    source = tmp_path / 'source.cpp'
    source.write_text('int value;\n')
    include = tmp_path / 'include path'
    database = build / 'compile_commands.json'
    argv = ['C:\\Program Files\\MSVC\\cl.exe', '/I' + str(include),
            '/Fo' + str(build / 'source.obj'), '/c', str(source)]
    database.write_text(json.dumps([{'directory': str(build), 'file': str(source),
                                    'command': subprocess.list2cmdline(argv)}]))
    entry = sonar.compiler_entries(database, tmp_path)[0]
    assert entry['target'] == 'source.obj'
    assert include in entry['includes']


def test_compiler_explicit_output_cannot_escape_build(tmp_path):
    build = tmp_path / 'build'
    build.mkdir()
    source = tmp_path / 'source.cpp'
    source.write_text('int value;\n')
    database = build / 'compile_commands.json'
    database.write_text(json.dumps([{'directory': str(build), 'file': str(source),
                                    'arguments': ['compiler'], 'output': '../outside.obj'}]))
    with pytest.raises(AnalysisError, match='in-build object'):
        sonar.compiler_entries(database, tmp_path)


@pytest.mark.skipif(sys.platform != 'win32', reason='Windows VS Code installation default')
def test_cli_uses_windows_roaming_code_profile(monkeypatch, tmp_path):
    monkeypatch.setenv('APPDATA', str(tmp_path / 'Roaming'))
    args = preflight.arguments(['doctor', '--offline'])
    assert args.code_user_data == tmp_path / 'Roaming' / 'Code'


def test_secrets_uses_current_finding_exit_without_retaining_sensitive_text(tmp_path, monkeypatch):
    monkeypatch.setattr(tools, 'quality_run', lambda *_: SimpleNamespace(returncode=51, stdout='private-value', stderr=''))
    result = tools.secrets(tmp_path, [Path('a.cpp')], 2)
    assert result['status'] == 'issues_found'
    assert 'private-value' not in json.dumps(result)


def test_secrets_auth_failure_is_not_misreported_as_findings(tmp_path, monkeypatch):
    monkeypatch.setattr(tools, 'quality_run', lambda *_: SimpleNamespace(returncode=1, stdout='private-value', stderr='auth failed'))
    with pytest.raises(AnalysisError, match='exit 1') as error:
        tools.secrets(tmp_path, [Path('a.cpp')], 2)
    assert 'private-value' not in str(error.value)


def test_changed_line_annotation_normalizes_native_relative_path(tmp_path):
    native = str(Path('src') / 'a.cpp')
    result = preflight.annotate([{'filePath': 'src/a.cpp', 'line': 3}], tmp_path, {native: [(3, 3)]})
    assert result[0]['changedLine'] is True
    assert result[0]['filePath'] == 'src/a.cpp'


@pytest.mark.skipif(sys.platform != 'win32', reason='Native managed Windows runtime')
def test_default_windows_context_does_not_force_linux_only_composition(tmp_path, monkeypatch):
    if sys.platform != 'win32':
        pytest.skip('Windows native default composition')
    root = tmp_path / 'root'
    root.mkdir()
    cache = tmp_path / 'cache'
    build = cache / 'build'
    build.mkdir(parents=True)
    (build / 'compile_commands.json').write_text('[]')
    (build / 'CMakeCache.txt').write_text('CMAKE_GENERATOR:INTERNAL=Ninja\n')
    calls = []
    monkeypatch.setattr(sonar, 'quality_run', lambda argv, *_: calls.append(argv) or SimpleNamespace(returncode=0, stdout='', stderr=''))
    sonar.prepare(root, cache, [], None, 2)
    assert '-DHORO_BUILD_RENDER_VULKAN=ON' not in calls[0]
    assert '-DHORO_ENABLE_IMGUI_UI_TESTS=ON' not in calls[0]
    assert '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON' in calls[0]


def test_lizard_finds_managed_native_windows_runtime(tmp_path, monkeypatch):
    executable = tmp_path / '.codacy/runtimes/lizard-1/venv/Scripts/lizard.exe'
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b'fixture')
    monkeypatch.setattr(tools.Path, 'home', lambda: tmp_path)
    monkeypatch.setattr(tools.shutil, 'which', lambda *_: None)
    assert tools.lizard_executable() == executable


@pytest.mark.skipif(sys.platform != 'win32', reason='Native PMD batch entry point')
def test_cpd_selects_native_batch_launcher_without_shell_script(tmp_path, monkeypatch):
    executable = tmp_path / '.codacy/tools/PMD/pmd-bin-6.55.0/bin/cpd.bat'
    executable.parent.mkdir(parents=True)
    executable.write_text('@rem fixture\n')
    monkeypatch.setattr(tools.Path, 'home', lambda: tmp_path)
    monkeypatch.setattr(tools, 'repository_files', lambda *_: [Path('a.cpp')])
    calls = []
    monkeypatch.setattr(tools, 'quality_run', lambda argv, *_: calls.append(argv) or SimpleNamespace(
        returncode=0, stdout='<pmd-cpd/>', stderr=''))
    mirror = tmp_path / 'mirror'
    mirror.mkdir()
    result = tools.duplication(tmp_path, mirror, [Path('a.cpp')], {}, {'PMD': {'installedVersion': '6.55.0'}}, 2)
    assert result['status'] == 'complete'
    assert calls[0][0] == str(executable)
    assert calls[0][1] == '--minimum-tokens'


def test_windows_doctor_reaches_dispatch_without_linux_guard(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(preflight, 'run_git', lambda *_: str(tmp_path).encode())
    monkeypatch.setattr(preflight, 'cache_directory', lambda *_: tmp_path / 'cache')
    monkeypatch.setattr(preflight, 'doctor', lambda *_: {'status': 'ready_for_local_check', 'exitCode': 0})
    if sys.platform != 'win32':
        pytest.skip('Actual Windows entry-point dispatch')
    assert preflight.preflight_main(['doctor', '--offline', '--worktree', str(tmp_path), '--format', 'json']) == 0
    assert json.loads(capsys.readouterr().out)['status'] == 'ready_for_local_check'
