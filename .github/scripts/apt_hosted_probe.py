#!/usr/bin/env python3
"""One-shot hosted read-only diagnostics; no environment/config dump or dpkg writes."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

OUT = Path(os.environ['RUNNER_TEMP']) / 'apt-hosted-evidence'
OUT.mkdir(exist_ok=True)
MODE = sys.argv[1]
SEP = sys.argv.index('--packages')
OPTIONS = sys.argv[2:SEP]
PACKAGES = sys.argv[SEP + 1:]
MAX_BYTES = 2 * 1024 * 1024


def redact(text):
    return re.sub(r'(https?://)[^\s/]*@', r'\1[REDACTED]@', text)


def command(name, args, seconds=30):
    start = time.monotonic()
    try:
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=seconds, check=False, env={**os.environ, 'LC_ALL': 'C'})
        code, raw = result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        code, raw = 124, error.stdout or b''
    text = redact(raw.decode('utf-8', errors='replace'))
    encoded = text.encode()
    truncated = len(encoded) > MAX_BYTES
    (OUT / (name + '.txt')).write_bytes(encoded[:MAX_BYTES])
    (OUT / (name + '.json')).write_text(json.dumps({
        'exit': code, 'elapsed_seconds': time.monotonic() - start,
        'original_output_bytes': len(encoded), 'truncated': truncated,
        'operation': 'read-only', 'argv': args,
    }, indent=2))
    return text


def metadata(label):
    identity = {key: os.environ.get(key) for key in ('ImageOS', 'ImageVersion',
                'RUNNER_OS', 'RUNNER_ARCH', 'GITHUB_SHA', 'GITHUB_REF',
                'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')}
    identity['os_release'] = Path('/etc/os-release').read_text()
    machine = Path('/etc/machine-id')
    identity['machine_id_sha256'] = hashlib.sha256(machine.read_bytes()).hexdigest() if machine.exists() else None
    (OUT / (label + '-identity.json')).write_text(json.dumps(identity, indent=2))
    command(label + '-apt-version', ['apt-get', '--version'])
    command(label + '-holds', ['apt-mark', 'showhold'])
    fields = ('${binary:Package}\t${Status}\t${Version}\t${Architecture}\t${Installed-Size}'
              '\t${Essential}\t${Multi-Arch}\t${Source}\t${Pre-Depends}\t${Depends}'
              '\t${Breaks}\t${Conflicts}\t${Replaces}\t${Provides}\n')
    command(label + '-installed-metadata', ['dpkg-query', '-W', '-f=' + fields])
    keys = ['APT::Default-Release', 'APT::Solver',
            'APT::Get::Always-Include-Phased-Updates',
            'APT::Get::Never-Include-Phased-Updates', 'APT::Get::Allow-Downgrades']
    values = {}
    for key in keys + ['APT::Machine-ID']:
        result = subprocess.run(['apt-config', *OPTIONS, 'shell', 'VALUE', key],
                                capture_output=True, check=False, timeout=5)
        value = result.stdout.decode(errors='replace').strip()
        if key == 'APT::Machine-ID':
            values[key + '-sha256'] = hashlib.sha256(value.encode()).hexdigest() if value else None
        elif len(value) < 256 and re.fullmatch(r"[A-Za-z0-9_:=;.'\"/ -]*", value):
            values[key] = value or '(unset)'
        else:
            values[key] = '[REDACTED unsupported value]'
    (OUT / (label + '-whitelisted-config.json')).write_text(json.dumps(values, indent=2))
    preferences = []
    files = [Path('/etc/apt/preferences')]
    directory = Path('/etc/apt/preferences.d')
    if directory.is_dir():
        files += sorted(p for p in directory.iterdir() if p.is_file())[:100]
    for index, path in enumerate(files):
        if not path.is_file():
            continue
        records = []
        for line in path.read_text(errors='replace')[:262144].splitlines():
            match = re.match(r'^(Package|Pin|Pin-Priority):\s*(.*?)\s*$', line)
            if not match:
                continue
            key, value = match.groups()
            safe = (re.fullmatch(r'[A-Za-z0-9*+?.:!~^$|()=, /_-]{1,512}', value)
                    and not any(token in value for token in ('://', '@', '\\')))
            records.append({key: value if safe else '[REDACTED unsupported value]'})
        preferences.append({'file_index': index, 'records': records})
    (OUT / (label + '-whitelisted-preferences.json')).write_text(json.dumps(preferences, indent=2))


metadata(MODE)
if MODE == 'failure':
    base = ['apt-get', *OPTIONS]
    install = ['--simulate', '--yes', '--no-install-recommends', 'install', *PACKAGES]
    debug = ['-o', 'Debug::pkgProblemResolver=true', '-o', 'Debug::pkgDepCache::AutoInstall=true']
    original = command('solver-baseline', base + debug + install)
    affected = set(PACKAGES)
    for match in re.finditer(r'^Inst ([a-z0-9][a-z0-9+.:~-]*)', original, re.M):
        affected.add(match.group(1))
    affected = sorted(affected)[:512]
    command('private-policy', ['apt-cache', *OPTIONS, 'policy', *affected])
    command('signed-package-metadata', ['apt-cache', *OPTIONS, 'show', *affected])
    # Controls affect only a --simulate process; never persisted or installed.
    empty = OUT / 'empty-preferences'
    empty.write_text('')
    empty_parts = OUT / 'empty-preferences.d'
    empty_parts.mkdir(exist_ok=True)
    controls = {
        'empty-preferences': ['-o', 'Dir::Etc::preferences=' + str(empty),
                              '-o', 'Dir::Etc::preferencesparts=' + str(empty_parts)],
        'include-phased': ['-o', 'APT::Get::Always-Include-Phased-Updates=true'],
        'solver-internal': ['-o', 'APT::Solver=internal'],
        'solver-3.0': ['-o', 'APT::Solver=3.0'],
    }
    for name, options in controls.items():
        command(name, base + options + debug + install)
    metadata('after-simulations')
print('Hosted diagnostic files captured; installer result remains separate.')
