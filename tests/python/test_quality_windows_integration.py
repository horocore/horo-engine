"""Portable CLI, compiler-context and native analyzer regression cases."""
from __future__ import annotations

import json
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
    # Independent compiler command fixture exercises quoting rather than a paired encoder.
    command = f'"C:\\Program Files\\MSVC\\cl.exe" "/I{include}" "/Fo{build / "source.obj"}" /c "{source}"'
    database.write_text(json.dumps([{'directory': str(build), 'file': str(source),
                                    'command': command}]))
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


def test_large_windows_secret_scan_preserves_every_path_and_redacts_all_batches(tmp_path, monkeypatch):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/sonar.exe')
    selected = [Path('src') / ('file_%04d_雪_😀_' % i + 'x' * 100 + '.cpp') for i in range(1000)]
    submitted = []
    calls = []

    def scan(argv, *_):
        command = tools._format_command('C:/quality/sonar.exe', argv[1:])
        assert len(tools.subprocess.list2cmdline(command).encode('utf-16-le')) // 2 + 1 <= 32767
        calls.append(argv)
        submitted.extend(argv[3:])
        return SimpleNamespace(returncode=51 if len(calls) == 1 else 0,
                               stdout='private-value' if len(calls) == 1 else 'No secrets found', stderr='')

    monkeypatch.setattr(tools, 'quality_run', scan)
    result = tools.secrets(tmp_path, selected, 30)
    assert len(calls) > 1
    assert submitted == [str(tmp_path / path) for path in selected]
    assert result['submitted'] == list(map(str, selected))
    assert result['status'] == 'issues_found'
    assert 'private-value' not in json.dumps(result)


@pytest.mark.parametrize('later_failure', [False, True])
def test_large_windows_codacy_scan_checks_each_batch_and_keeps_late_failures(tmp_path, monkeypatch, later_failure):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/codacy-analysis.cmd')
    selected = [Path('src') / ('file_%04d_' % i + 'x' * 100 + ('.cpp' if i < 500 else '.py')) for i in range(1000)]
    submitted = []
    calls = []

    def analyze(argv, *_):
        command = tools._format_command('C:/quality/codacy-analysis.cmd', argv[1:])
        assert len(command.encode('utf-16-le')) // 2 + 1 <= 8191
        batch = argv[argv.index('--files') + 1:]
        calls.append(batch)
        submitted.extend(batch)
        tools_in_batch = [argv[i + 1] for i, value in enumerate(argv) if value == '--tool']
        results = [{'toolId': tool, 'status': 'success',
                    'filesAnalyzed': sum(path.endswith('.cpp' if tool == 'Cpp' else '.py') for path in batch)}
                   for tool in tools_in_batch]
        payload = {'issues': [{'filePath': batch[0], 'message': 'first batch finding'}] if len(calls) == 1 else [],
                   'errors': [], 'toolResults': results}
        return payload, 2 if later_failure and len(calls) == 2 else 0

    monkeypatch.setattr(tools, 'json_command', analyze)
    result = tools.codacy_check(tmp_path, tmp_path, selected,
                               {'tools': [{'toolId': 'Cpp'}, {'toolId': 'Python'}]},
                               {'Cpp': {'languages': ['CPP']}, 'Python': {'languages': ['Python']}}, 30)
    assert len(calls) > 1
    assert submitted == list(map(str, selected))
    assert result['status'] == ('incomplete' if later_failure else 'issues_found')
    assert result['issues'][0]['message'] == 'first batch finding'
    assert len(result['batches']) == len(calls)
    assert [path for batch in result['batches'] for path in batch['submitted']] == submitted


@pytest.mark.parametrize('second_code,second_text', [(1, 'private-value'), (0, 'private-value')])
def test_later_secret_batch_failure_cannot_be_hidden_by_an_earlier_finding(tmp_path, monkeypatch, second_code, second_text):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/sonar.exe')
    selected = [Path('src') / ('x' * 100 + str(i) + '.cpp') for i in range(1000)]
    responses = iter([SimpleNamespace(returncode=51, stdout='private-value', stderr=''),
                      SimpleNamespace(returncode=second_code, stdout=second_text, stderr='')])
    monkeypatch.setattr(tools, 'quality_run', lambda *_: next(responses))
    with pytest.raises(AnalysisError) as error:
        tools.secrets(tmp_path, selected, 30)
    assert 'private-value' not in str(error.value)


def test_windows_input_larger_than_one_command_fails_before_any_batch_launch(tmp_path, monkeypatch):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/sonar.exe')
    monkeypatch.setattr(tools, 'quality_run', lambda *_: pytest.fail('Oversized input must not launch a partial scan'))
    with pytest.raises(AnalysisError, match='One analysis input'):
        tools.secrets(tmp_path, [Path('small.cpp'), Path('x' * 33000)], 30)


@pytest.mark.skipif(sys.platform != 'win32', reason='Actual cmd launcher and native Windows argument limits')
def test_padded_windows_batch_launcher_preserves_every_argument(tmp_path):
    receiver = tmp_path / 'receiver.py'
    receiver.write_text('import json, sys\nprint(json.dumps(sys.argv[2:]))\n', encoding='utf-8')
    launcher = tmp_path / 'launcher.cmd'
    launcher.write_text('@echo off\nsetlocal\nset "QUALITY_PADDING=' + 'x' * 500 + '"\n'
                        f'"{sys.executable}" "{receiver}" "%QUALITY_PADDING%" %*\n', encoding='utf-8')
    selected = [Path('file_%04d_' % i + 'x' * 100 + '.cpp') for i in range(300)]
    batches = tools.path_batches([str(launcher)], selected)
    assert len(batches) > 1
    received = []
    for batch in batches:
        result = tools.quality_run([str(launcher), *map(str, batch)], tmp_path, 30)
        assert result.returncode == 0
        received.extend(json.loads(result.stdout))
    assert received == list(map(str, selected))


def test_single_windows_batch_argument_can_use_the_hard_limit(tmp_path, monkeypatch):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/codacy-analysis.cmd')
    selected = [Path('x' * 7300 + '.cpp')]
    assert tools.path_batches(['codacy-analysis', '--files'], selected) == [selected]


@pytest.mark.skipif(sys.platform != 'win32', reason='Actual native Windows child-process argument limit')
def test_native_windows_forwarder_preserves_every_argument(tmp_path):
    receiver = tmp_path / 'receiver.py'
    receiver.write_text('import json, sys\nprint(json.dumps(sys.argv[2:]))\n', encoding='utf-8')
    forwarder = tmp_path / 'forwarder.py'
    forwarder.write_text('import subprocess, sys\n'
                         'subprocess.run([sys.executable, sys.argv[1], "x" * 500, *sys.argv[2:]], check=True)\n',
                         encoding='utf-8')
    prefix = [sys.executable, str(forwarder), str(receiver)]
    selected = [Path('file_%04d_' % i + 'x' * 100 + '.cpp') for i in range(600)]
    batches = tools.path_batches(prefix, selected)
    assert len(batches) > 1
    received = []
    for batch in batches:
        result = tools.quality_run([*prefix, *map(str, batch)], tmp_path, 30)
        assert result.returncode == 0
        received.extend(json.loads(result.stdout))
    assert received == list(map(str, selected))


@pytest.mark.parametrize('change', [None, 'version', 'missing', 'failure'])
def test_batch_runtime_evidence_is_preserved_only_when_consistent(change):
    def report():
        return {'status': 'clean', 'issues': [], 'errors': [],
                'toolResults': [{'toolId': 'Lizard', 'status': 'success', 'filesAnalyzed': 1}],
                'capability': {'ready': [{'toolId': 'Lizard', 'version': '1.23.0', 'installation': 'codacy',
                                          'runner': 'local', 'filesRouted': 1}]}}

    first, second = report(), report()
    if change == 'version':
        second['capability']['ready'][0]['version'] = 'different'
    elif change == 'missing':
        second['capability']['ready'] = []
    elif change == 'failure':
        second.update(status='incomplete', errors=[{'toolId': 'Lizard', 'reason': 'missing_or_incomplete_execution'}])
    result = tools.combine_codacy_batches([first, second], [Path('first.cpp'), Path('second.cpp')])
    if change is None:
        assert result['capability']['ready'][0]['filesRouted'] == 2
        assert result['status'] == 'clean'
    else:
        assert not result['capability']['ready']
    if change in {'version', 'failure'}:
        assert result['status'] == 'incomplete'


@pytest.mark.parametrize('late_failure', [False, True])
def test_windows_raw_complexity_batches_keep_all_metrics_and_reject_partial_success(tmp_path, monkeypatch, late_failure):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/lizard.exe')
    monkeypatch.setattr(tools, 'lizard_executable', lambda *_: Path('C:/quality/lizard.exe'))
    selected = [Path('src') / ('file_%04d_' % i + 'x' * 100 + '.cpp') for i in range(1000)]
    submitted = []
    calls = []

    def run(argv, *_):
        if argv[1] == '--version':
            return SimpleNamespace(returncode=0, stdout='1.23.0', stderr='')
        calls.append(argv)
        assert len(tools.subprocess.list2cmdline(argv).encode('utf-16-le')) // 2 + 1 <= 32767
        if late_failure and len(calls) == 2:
            return SimpleNamespace(returncode=1, stdout='', stderr='failed')
        submitted.extend(argv[2:])
        output = tools.io.StringIO()
        writer = tools.csv.writer(output)
        for path in argv[2:]:
            writer.writerow([1, 2, 3, 4, 5, 6, path, 'function', 'unused', 1, 2])
        return SimpleNamespace(returncode=0, stdout=output.getvalue(), stderr='')

    monkeypatch.setattr(tools, 'quality_run', run)
    capability = {'ready': [{'toolId': 'Lizard', 'version': '1.23.0', 'installation': 'codacy'}]}
    if late_failure:
        with pytest.raises(AnalysisError, match='could not complete'):
            tools.complexity(tmp_path, selected, capability, 30)
    else:
        result = tools.complexity(tmp_path, selected, capability, 30)
        assert result['status'] == 'complete'
        assert len(result['files']) == len(selected)
        assert sum(item['cyclomaticComplexity'] for item in result['files']) == 2000
        assert submitted == [str(tmp_path / path) for path in selected]
    assert len(calls) > 1


def test_secret_batches_share_one_deadline_instead_of_resetting_the_timeout(tmp_path, monkeypatch):
    monkeypatch.setattr(tools.sys, 'platform', 'win32')
    monkeypatch.setattr(tools, '_command_executable', lambda *_: 'C:/quality/sonar.exe')
    clock = iter([0, 1, 6])
    monkeypatch.setattr(tools, 'time', SimpleNamespace(monotonic=lambda: next(clock)))
    calls = []
    monkeypatch.setattr(tools, 'quality_run', lambda argv, root, timeout: calls.append(timeout) or
                        SimpleNamespace(returncode=0, stdout='No secrets found', stderr=''))
    selected = [Path('src') / ('x' * 100 + str(i) + '.cpp') for i in range(1000)]
    with pytest.raises(AnalysisError, match='timed out before all input batches'):
        tools.secrets(tmp_path, selected, 5)
    assert calls == [4]


def test_complexity_row_from_a_different_selected_batch_is_rejected(tmp_path):
    totals = {name: {'path': name, 'cyclomaticComplexity': 0, 'functions': []} for name in ('first.cpp', 'second.cpp')}
    output = tools.io.StringIO()
    tools.csv.writer(output).writerow([1, 2, 3, 4, 5, 6, str(tmp_path / 'second.cpp'), 'function', 'unused', 1, 2])
    with pytest.raises(AnalysisError, match='outside the selected set'):
        tools.add_complexity_rows(output.getvalue(), tmp_path, totals, {'first.cpp'})
    assert all(item['cyclomaticComplexity'] == 0 for item in totals.values())


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


@pytest.mark.parametrize('os_name,relative', [('nt', 'Scripts/lizard.exe'), ('posix', 'bin/lizard')])
def test_lizard_finds_managed_native_runtime(tmp_path, monkeypatch, os_name, relative):
    executable = tmp_path / '.codacy/runtimes/lizard-1/venv' / relative
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b'fixture')
    monkeypatch.setattr(tools.Path, 'home', lambda: tmp_path)
    monkeypatch.setattr(tools, 'os', SimpleNamespace(name=os_name))
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
