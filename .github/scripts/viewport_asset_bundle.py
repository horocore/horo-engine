"""Create/verify a same-run data-only canonical viewport bundle; never execute producer tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

PAYLOAD = 'shaders/editor/viewport-vulkan.horoshdr'
REFLECTION = 'shaders/editor/viewport-vulkan.reflection.json'
CATALOG_SOURCE = 'src/runtime/renderer/toolchain/ShaderCompilerToolchainCatalog.cpp'
FILES = (PAYLOAD, REFLECTION, 'catalog.tsv')


def digest(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f'Missing, empty or symlink bundle input: {path}')
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_catalog(source):
    text = (source / CATALOG_SOURCE).read_text(encoding='utf-8')
    result = []
    for role, body in re.findall(r'ApprovedShaderCompilerTool\{ShaderCompilerTool::(\w+),\s*(.*?)\}', text, re.S):
        if role not in ('Dxc', 'SpirvTools', 'SpirvCross'):
            continue
        fields = [''.join(re.findall(r'"([^"\n]*)"', part)) for part in body.split(',')]
        if len(fields) != 7 or fields[0] != 'linux-x86_64-ubuntu-26.04':
            raise ValueError('Unexpected canonical tool catalog schema')
        number = ('Dxc', 'SpirvTools', 'SpirvCross').index(role)
        result.append('\t'.join((str(number), fields[2], fields[3], fields[4], fields[6])))
    if len(result) != 3:
        raise ValueError('Canonical tool catalog must contain exactly three viewport roles')
    return '\n'.join(result) + '\n'


def identity(environ):
    head, tree = environ['GITHUB_SHA'], environ['HORO_VIEWPORT_SOURCE_TREE']
    if not re.fullmatch('[0-9a-f]{40}', head) or not re.fullmatch('[0-9a-f]{40}', tree):
        raise ValueError('Invalid source SHA/tree identity')
    return {'sha': head, 'tree': tree,
            'run_id': environ['GITHUB_RUN_ID'], 'run_attempt': environ['GITHUB_RUN_ATTEMPT'],
            'repository': environ['GITHUB_REPOSITORY']}


def bundle_entries(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('Asset root absent or symlink')
    paths = tuple(root.rglob('*'))
    if any(p.is_symlink() for p in paths):
        raise ValueError('Symlinks forbidden in data-only bundle')
    return {p.relative_to(root).as_posix() for p in paths if not p.is_dir()}


def inspect(root, source):
    expected = set(FILES) | {'provenance.json'}
    actual = bundle_entries(root)
    if not set(FILES) <= actual or actual - expected:
        raise ValueError('Bundle must contain only payload, reflection, catalog and provenance')
    hashes = {name: digest(root / name) for name in FILES}
    if (root / 'catalog.tsv').read_text(encoding='utf-8') != canonical_catalog(source):
        raise ValueError('Bundle catalog differs from checkout canonical lock')
    json.loads((root / REFLECTION).read_text(encoding='utf-8'))
    return hashes


def create(root, source, expected):
    hashes = inspect(root, source)
    (root / 'provenance.json').write_text(json.dumps({'schema': 1, 'identity': expected, 'sha256': hashes},
                                                   indent=2) + '\n', encoding='utf-8')


def verify(root, source, expected):
    hashes = inspect(root, source)
    digest(root / 'provenance.json')
    actual = json.loads((root / 'provenance.json').read_text(encoding='utf-8'))
    if actual != {'schema': 1, 'identity': expected, 'sha256': hashes}:
        raise ValueError('Stale source/run identity or bundle digest mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('create', 'verify'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path.cwd())
    args = parser.parse_args()
    operation = create if args.operation == 'create' else verify
    operation(args.root.absolute(), args.source.resolve(), identity(os.environ))


if __name__ == '__main__':
    main()
