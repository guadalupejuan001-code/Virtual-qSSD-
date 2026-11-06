#!/usr/bin/env python3
"""Obtain the user's original cores at reviewed revisions; never modify them."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
for name, entry in json.loads((root / 'dependencies.json').read_text()).items():
    target = root / 'deps' / name
    if not (target / '.git').exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', str(target)], check=True)
        subprocess.run(['git', '-C', str(target), 'remote', 'add', 'origin',
                        'https://github.com/' + entry['repository'] + '.git'], check=True)
    actual = subprocess.run(['git', '-C', str(target), 'rev-parse', 'HEAD'], text=True,
                            capture_output=True).stdout.strip()
    if actual != entry['commit']:
        subprocess.run(['git', '-C', str(target), 'fetch', '--depth', '1', 'origin', entry['commit']], check=True)
        subprocess.run(['git', '-C', str(target), 'checkout', '--detach', entry['commit']], check=True)
    print(name, entry['commit'])
