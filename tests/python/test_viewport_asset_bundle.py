"""Same-run data-only viewport provisioning, independent of coverage selection."""
from pathlib import Path
import importlib.util
import json
import os
import re
import sys
import pytest

pytest_plugins = ("pytester",)

ROOT = Path(__file__).resolve().parents[2]


def test_sonar_cooks_and_verifies_locally_after_build_before_coverage():
    workflow = (ROOT / '.github/workflows/sonar.yml').read_text()
    assert '  viewport-assets:' not in workflow
    consumer = workflow.split('  sonar:', 1)[1]
    assert re.search(r'^    runs-on: (?:&sonar_runner )?ubuntu-26\.04$', consumer, re.MULTILINE)
    assert 'needs:' not in consumer
    assert 'actions: read' not in consumer
    assert 'download-artifact' not in consumer
    assert 'waitForViewportArtifact' not in consumer
    assert 'cmake --build --preset sonar --target all HoroEditorVulkanShaderCook HoroEditorUiPaintShaderCook' in consumer
    assert 'uses: ./.github/actions/cook-vulkan-viewport' in consumer
    assert 'build-directory: build/sonar' in consumer
    assert 'asset-root: ${{ github.workspace }}/build/sonar/native-editor-assets' in consumer
    require_provisioning_order(consumer)
    assert '[[ "$(git rev-parse HEAD)" == "$GITHUB_SHA" ]]' in consumer
    assert "HORO_VIEWPORT_SOURCE_TREE=" in consumer
    assert 'contents: read' in workflow
    python = consumer.index('uses: actions/setup-python@a26af69be951a213d495a4c3e4e4022e16d87065')
    venv = consumer.index('python3 -m venv /tmp/horo-coverage')
    assert python < venv
    assert "python-version: '3.12'" in consumer[python:venv]
    assert 'coverage==7.6.10 fastcov==1.17 pytest==8.3.4 defusedxml==0.7.1' in consumer
    assert '-DPython3_EXECUTABLE=/tmp/horo-coverage/bin/python' in consumer
    require_provisioning_failure_policy(consumer)


def require_provisioning_failure_policy(consumer):
    # Only these cache accelerators may fail before coverage starts.
    prefix = consumer.split('      - name: Run Python coverage tests', 1)[0]
    header, *steps = re.split(r'^      - ', prefix, flags=re.MULTILINE)
    assert 'continue-on-error' not in header
    optional = {
        'name: Install optional compiler registry client': '        uses: oras-project/setup-oras@',
        'name: Restore optional public compiler checkpoint':
            '        run: python3 .github/scripts/compiler_registry.py fetch --key ',
        'name: Publish main instrumented compiler checkpoint':
            '        uses: ./.github/actions/publish-compiler-checkpoint',
    }
    for step in steps:
        name = step.partition('\n')[0]
        if name in optional:
            assert any(line.startswith(optional[name]) for line in step.splitlines())
        else:
            assert 'continue-on-error' not in step


def test_provisioning_failure_policy_accepts_optional_compiler_checkpoints():
    consumer = (ROOT / '.github/workflows/sonar.yml').read_text().split('  sonar:', 1)[1]
    require_provisioning_failure_policy(consumer)


@pytest.mark.parametrize('name', [
    'Check Sonar authentication', 'Setup build tools', 'Install coverage tools',
    'Configure instrumented Debug build', 'Build instrumented targets',
    'Cook canonical viewport assets for coverage',
    'Verify and select viewport data for coverage', 'Run C++ coverage tests',
])
def test_provisioning_failure_policy_rejects_ignored_required_step(name):
    consumer = (ROOT / '.github/workflows/sonar.yml').read_text().split('  sonar:', 1)[1]
    require_provisioning_failure_policy(consumer)
    marker = f'      - name: {name}\n'
    mutated = consumer.replace(marker, marker + '        continue-on-error: true\n', 1)
    assert mutated != consumer
    with pytest.raises(AssertionError):
        require_provisioning_failure_policy(mutated)


def test_provisioning_failure_policy_rejects_ignored_job():
    consumer = (ROOT / '.github/workflows/sonar.yml').read_text().split('  sonar:', 1)[1]
    require_provisioning_failure_policy(consumer)
    mutated = consumer.replace('    name: SonarCloud\n',
                               '    name: SonarCloud\n    continue-on-error: true\n', 1)
    assert mutated != consumer
    with pytest.raises(AssertionError):
        require_provisioning_failure_policy(mutated)


def test_provisioning_failure_policy_rejects_unrelated_optional_step():
    consumer = (ROOT / '.github/workflows/sonar.yml').read_text().split('  sonar:', 1)[1]
    require_provisioning_failure_policy(consumer)
    marker = '      - name: Build instrumented targets\n'
    extra = ('      - name: Unrelated optional setup\n'
             '        continue-on-error: true\n        run: echo fixture\n\n')
    mutated = consumer.replace(marker, extra + marker, 1)
    assert mutated != consumer
    with pytest.raises(AssertionError):
        require_provisioning_failure_policy(mutated)


@pytest.mark.parametrize('original,replacement', [
    ('uses: oras-project/setup-oras@', 'uses: different/action@'),
    ('run: python3 .github/scripts/compiler_registry.py fetch --key ', 'run: echo '),
    ('uses: ./.github/actions/publish-compiler-checkpoint', 'uses: different/action'),
])
def test_provisioning_failure_policy_rejects_repurposed_optional_step(original, replacement):
    consumer = (ROOT / '.github/workflows/sonar.yml').read_text().split('  sonar:', 1)[1]
    require_provisioning_failure_policy(consumer)
    mutated = consumer.replace(original, replacement, 1)
    assert mutated != consumer
    with pytest.raises(AssertionError):
        require_provisioning_failure_policy(mutated)


def require_provisioning_order(consumer):
    build = consumer.index('name: Build instrumented targets')
    checkpoint = consumer.index('name: Publish main instrumented compiler checkpoint')
    cook = consumer.index('uses: ./.github/actions/cook-vulkan-viewport')
    verify = consumer.index('viewport_asset_bundle.py verify')
    export = consumer.index('HORO_EDITOR_VULKAN_ASSET_ROOT=')
    coverage = consumer.index('ctest --preset sonar')
    assert build < checkpoint < cook < verify < export < coverage


@pytest.mark.parametrize('steps', [
    ['ctest --preset sonar', 'viewport_asset_bundle.py verify', 'HORO_EDITOR_VULKAN_ASSET_ROOT='],
    ['HORO_EDITOR_VULKAN_ASSET_ROOT=', 'viewport_asset_bundle.py verify', 'ctest --preset sonar'],
])
def test_provisioning_contract_rejects_wrong_order(steps):
    prefix = ['name: Build instrumented targets', 'name: Publish main instrumented compiler checkpoint',
              'uses: ./.github/actions/cook-vulkan-viewport']
    with pytest.raises(AssertionError):
        require_provisioning_order('\n'.join(prefix + steps))


@pytest.fixture
def bundle(tmp_path):
    spec = importlib.util.spec_from_file_location('viewport_asset_bundle', ROOT / '.github/scripts/viewport_asset_bundle.py')
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    root = tmp_path / 'assets'
    (root / 'shaders/editor').mkdir(parents=True)
    # Deliberately synthetic protocol fixtures, not claimed as native cooked assets.
    (root / module.PAYLOAD).write_bytes(b'protocol fixture payload')
    (root / module.REFLECTION).write_text('{}')
    (root / 'catalog.tsv').write_text(module.canonical_catalog(ROOT))
    expected = {'sha': 'a' * 40, 'tree': 'b' * 40, 'run_id': '123', 'run_attempt': '1', 'repository': 'fixture/repo'}
    module.create(root, ROOT, expected)
    return module, root, expected


def test_bundle_accepts_exact_same_source_run_and_data(bundle):
    module, root, expected = bundle
    module.verify(root, ROOT, expected)


@pytest.mark.parametrize('field', ['sha', 'tree', 'run_id', 'run_attempt', 'repository'])
def test_bundle_rejects_stale_source_or_other_run(bundle, field):
    module, root, expected = bundle
    wrong = dict(expected, **{field: 'stale'})
    with pytest.raises(ValueError, match='Stale'):
        module.verify(root, ROOT, wrong)


@pytest.mark.parametrize('name', ['shaders/editor/viewport-vulkan.horoshdr',
                                  'shaders/editor/viewport-vulkan.reflection.json', 'catalog.tsv'])
def test_bundle_rejects_missing_required_data(bundle, name):
    module, root, expected = bundle
    (root / name).unlink()
    with pytest.raises(ValueError, match='Bundle must contain'):
        module.verify(root, ROOT, expected)


def test_bundle_rejects_changed_payload_hash(bundle):
    module, root, expected = bundle
    (root / module.PAYLOAD).write_bytes(b'tampered fixture')
    with pytest.raises(ValueError, match='digest mismatch'):
        module.verify(root, ROOT, expected)


def test_bundle_rejects_catalog_substitution_even_with_rehashed_provenance(bundle):
    module, root, expected = bundle
    (root / 'catalog.tsv').write_text('arbitrary tool catalog')
    provenance = json.loads((root / 'provenance.json').read_text())
    provenance['sha256']['catalog.tsv'] = module.digest(root / 'catalog.tsv')
    (root / 'provenance.json').write_text(json.dumps(provenance))
    with pytest.raises(ValueError, match='canonical lock'):
        module.verify(root, ROOT, expected)


def test_bundle_rejects_absent_asset_root(bundle):
    module, root, expected = bundle
    with pytest.raises(ValueError, match='root absent'):
        module.verify(root / 'missing', ROOT, expected)


def test_bundle_rejects_executable_or_other_extra_files(bundle):
    module, root, expected = bundle
    (root / 'producer-executable').write_bytes(b'not allowed')
    with pytest.raises(ValueError, match='Bundle must contain only'):
        module.verify(root, ROOT, expected)


@pytest.mark.skipif(os.name != 'posix', reason='Sonar coverage uses POSIX system Bash')
@pytest.mark.parametrize('invalid', [None, 'payload', 'run'])
def test_local_verification_exports_root_only_for_current_unmodified_bundle(bundle, tmp_path, invalid, pytester, monkeypatch):
    module, root, expected = bundle
    environment = tmp_path / 'github-env'
    environment.write_text('previous=value\n')
    if invalid == 'payload':
        (root / module.PAYLOAD).write_bytes(b'changed after local cook')
    identity = dict(expected, run_id='foreign') if invalid == 'run' else expected
    env = {'GITHUB_ENV': str(environment), 'GITHUB_SHA': identity['sha'],
           'HORO_VIEWPORT_SOURCE_TREE': identity['tree'], 'GITHUB_RUN_ID': identity['run_id'],
           'GITHUB_RUN_ATTEMPT': identity['run_attempt'], 'GITHUB_REPOSITORY': identity['repository']}
    for key, value in env.items():
        monkeypatch.setenv(key, value)
    # Resolve the fixed system tool, never an executable supplied through PATH or bundle contents.
    bash = Path('/bin/bash').resolve(strict=True)
    interpreter = Path(sys.executable).resolve(strict=True)
    assert bash.is_absolute() and os.access(bash, os.X_OK)
    # Match the workflow's fail-closed verify -> export boundary without invoking any native tool.
    script = '''set -euo pipefail
"$1" "$2" verify --root "$3" --source "$4"
printf 'HORO_EDITOR_VULKAN_ASSET_ROOT=%s\n' "$3" >> "$GITHUB_ENV"
'''
    result = pytester.run(str(bash), '-c', script, 'local-viewport-fixture', str(interpreter),
                          str(ROOT / '.github/scripts/viewport_asset_bundle.py'), str(root), str(ROOT), timeout=10)
    if invalid:
        assert result.ret != 0
        assert environment.read_text() == 'previous=value\n'
    else:
        assert result.ret == 0, result.stderr.str()
        assert environment.read_text() == f'previous=value\nHORO_EDITOR_VULKAN_ASSET_ROOT={root}\n'
