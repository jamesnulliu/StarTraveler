"""Verify an extracted team package against its own relative-path inventory."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
manifests = list(root.glob('PACKAGE-MANIFEST-*.json'))
if not manifests:
    raise SystemExit('Run this copy inside an extracted handoff package.')
for manifest in manifests:
    data = json.loads(manifest.read_text(encoding='utf-8'))
    for relative, expected in data['files'].items():
        target = (root / relative).resolve()
        if not target.is_relative_to(root):
            raise SystemExit(f'Invalid inventory path: {relative}')
        if not target.is_file() or target.stat().st_size != expected['bytes']:
            raise SystemExit(f'Missing or wrong size: {relative}')
        h = hashlib.sha256()
        with target.open('rb') as stream:
            for block in iter(lambda: stream.read(1024*1024), b''):
                h.update(block)
        if h.hexdigest() != expected['sha256']:
            raise SystemExit(f'Hash mismatch: {relative}')
    print(f"PASS: {data['kind']}, {len(data['files'])} files")
