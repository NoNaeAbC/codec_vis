#!/usr/bin/env python3
"""Fetch the locked upstream source and Arch patches for one static library."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

name = sys.argv[1]
entry = json.loads(Path('packaging/ci/arch-sources.json').read_text())[name]
base = Path(os.environ.get('CODEC_VIS_SOURCES', '/opt/codec-vis-sources')) / 'arch'
base.mkdir(parents=True, exist_ok=True)
target = base / name
fingerprint = hashlib.sha256(json.dumps(entry, sort_keys=True).encode()).hexdigest()
stamp = target / '.codec-vis-source'
if stamp.is_file() and stamp.read_text().strip() == fingerprint:
    print(target)
    sys.exit(0)


def download(spec):
    algorithm, expected = spec['hash'].split(':')
    archive = base / (expected + '.download')
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        subprocess.run(['curl', '--fail', '--location', '--retry', '3',
                        '--output', str(partial), spec['url']], check=True)
        partial.replace(archive)
    with archive.open('rb') as f:
        actual = hashlib.file_digest(f, algorithm).hexdigest()
    if actual != expected:
        raise SystemExit(f'Checksum mismatch for {spec["url"]}')
    return archive


with tempfile.TemporaryDirectory(dir=base) as temporary:
    work = Path(temporary)
    if 'git' in entry:
        source = work / 'source'
        subprocess.run(['git', 'init', str(source)], check=True, stdout=sys.stderr)
        subprocess.run(['git', '-C', str(source), 'fetch', '--depth=1', entry['git'], entry['commit']], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', 'FETCH_HEAD'], check=True, stdout=sys.stderr)
    else:
        subprocess.run(['tar', '-xf', str(download(entry)), '-C', str(work)], check=True)
        directories = [p for p in work.iterdir() if p.is_dir()]
        if len(directories) != 1:
            raise SystemExit(f'Expected one source directory for {name}')
        source = directories[0]
    for patch in entry.get('patches', []):
        subprocess.run(['patch', '--batch', '-p1', '-i', str(download(patch))], cwd=source, check=True, stdout=sys.stderr)
    if target.exists():
        shutil.rmtree(target)
    shutil.move(str(source), target)
    stamp.write_text(fingerprint + '\n')
print(target)
