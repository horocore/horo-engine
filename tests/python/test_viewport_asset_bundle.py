"""Same-run data-only viewport provisioning, independent of coverage selection."""
from pathlib import Path
import importlib.util
import json
import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_sonar_provisions_verified_viewport_assets_before_coverage():
    workflow = (ROOT / '.github/workflows/sonar.yml').read_text()
    assert '  viewport-assets:' in workflow
    producer, consumer = workflow.split('  sonar:', 1)
    assert 'runs-on: ubuntu-26.04' in producer
    assert 'needs: viewport-assets' in consumer
    assert 'runs-on: ubuntu-24.04' in consumer
    assert "github.event.pull_request.head.repo.full_name == github.repository" in producer
    assert 'HoroEditorVulkanShaderCook --parallel 2' in producer
    assert 'uses: ./.github/actions/cook-vulkan-viewport' in producer
    assert 'artifact-ids: ${{ needs.viewport-assets.outputs.artifact-id }}' in consumer
    assert 'run-id:' not in consumer
    require_provisioning_order(consumer)
    assert 'CC: gcc' in producer and 'CXX: g++' in producer
    assert '[[ "$(git rev-parse HEAD)" == "$GITHUB_SHA" ]]' in consumer
    assert "HORO_VIEWPORT_SOURCE_TREE=" in consumer
    assert 'contents: read' in workflow
    assert 'continue-on-error' not in producer


def require_provisioning_order(consumer):
    verify = consumer.index('viewport_asset_bundle.py verify')
    export = consumer.index('HORO_EDITOR_VULKAN_ASSET_ROOT=')
    coverage = consumer.index('ctest --preset sonar')
    assert verify < export < coverage


@pytest.mark.parametrize('steps', [
    ['ctest --preset sonar', 'viewport_asset_bundle.py verify', 'HORO_EDITOR_VULKAN_ASSET_ROOT='],
    ['HORO_EDITOR_VULKAN_ASSET_ROOT=', 'viewport_asset_bundle.py verify', 'ctest --preset sonar'],
])
def test_provisioning_contract_rejects_wrong_order(steps):
    with pytest.raises(AssertionError):
        require_provisioning_order('\n'.join(steps))


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
