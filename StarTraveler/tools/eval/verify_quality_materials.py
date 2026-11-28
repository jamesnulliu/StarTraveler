"""Compare R2 report, editable chart caches and notes with frozen study data."""
import csv
import hashlib
import json
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'docs/final-materials'
shots = ['cockpit-anvil-turn', 'mirror-glass-pass', 'fast-star-turn']
methods = ['asvgf-dlss-full', 'asvgf-dlss-half', 'rr-quality']
raw = (ROOT / 'renders/quality-fixed-exposure/summary.csv').read_bytes()
assert raw == (OUT / 'quality-chart-data.csv').read_bytes()
rows = list(csv.DictReader(raw.decode().splitlines()))
lookup = {(r['shot'], r['config']): r for r in rows}
report = (ROOT / 'docs/quality-fixed-exposure-results.md').read_text(encoding='utf-8')
cells = [[c.strip() for c in line.strip('|').split('|')] for line in report.splitlines()
         if line.startswith(('| Anvil cockpit |', '| Mirror / glass |', '| Star turn |'))]
assert len(cells) == 9
for row, source in zip(cells, rows):
    assert row[2] == f"{float(source['gpu_ms_median']):.3f} ({float(source['gpu_ms_min']):.3f}–{float(source['gpu_ms_max']):.3f})"
    assert row[3] == f"{float(source['trace_ms_median']):.3f}"
    assert row[4] == f"{float(source['flip_median']):.5f}"
    assert row[5] == f"{float(source['secondary_flip_median']):.5f}"
    assert row[6] == '3'
slides = json.loads((OUT / 'slides-r2.json').read_text(encoding='utf-8'))
assert len(slides) == 14 and sum(s['seconds'] for s in slides) == 720
script = (OUT / 'speaker-script-r2.md').read_text(encoding='utf-8')
assert all(s['script'] in script and s['cue'] in script for s in slides)
pptx = OUT / 'StarTraveler-denoiser-presentation-r2.pptx'
ns = {'c': 'http://schemas.openxmlformats.org/drawingml/2006/chart',
      'a': 'http://schemas.openxmlformats.org/drawingml/2006/main'}
checked = 0
with zipfile.ZipFile(pptx) as z:
    for index, metric in [(3, 'gpu_ms_median'), (4, 'flip_median')]:
        tree = ET.fromstring(z.read(f'ppt/slides/charts/chart{index}.xml'))
        series = tree.findall('.//c:ser', ns)
        assert len(series) == 3
        for ser, method in zip(series, methods):
            values = [float(v.text) for v in ser.findall('./c:val//c:pt/c:v', ns)]
            expected = [round(float(lookup[s, method][metric]), 8) for s in shots]
            assert values == expected, (values, expected)
            checked += len(values)
    for i, slide in enumerate(slides, 1):
        notes = ''.join(t.text or '' for t in ET.fromstring(z.read(
            f'ppt/notesSlides/notesSlide{i}.xml')).findall('.//a:t', ns))
        assert slide['script'] in notes and slide['cue'] in notes
receipt = ROOT / '.final-materials-build/r2/presentation-final.validation.json'
assert json.loads(receipt.read_text())['finalSha256'] == hashlib.sha256(pptx.read_bytes()).hexdigest()
shutil.copy2(receipt, OUT / 'presentation-r2-validation.json')
result = {'passed': True, 'quality_report_rows': 9, 'new_chart_values': checked,
          'notes_checked': 14, 'main_speech_seconds': 720,
          'visual_review': 'All 14 full-size slide renders and dual-reference figure reviewed; no clipping observed.',
          'native_powerpoint_tested': False,
          'presentation_sha256': hashlib.sha256(pptx.read_bytes()).hexdigest()}
(OUT / 'quality-materials-verification.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result))
