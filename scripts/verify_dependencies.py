import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
for name, entry in json.loads((root / 'dependencies.json').read_text()).items():
    actual = subprocess.run(['git', '-C', str(root / 'deps' / name), 'rev-parse', 'HEAD'],
                            capture_output=True, text=True, check=True).stdout.strip()
    if actual != entry['commit']:
        raise SystemExit(f'Unreviewed core: {name}: {actual}')
