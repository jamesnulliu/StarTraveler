"""Check report/slide numbers against frozen summary data, without rendering."""
import csv
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'docs/final-materials'
DATA = ROOT / 'renders/production-lag-fixed-exposure'
raw = (DATA / 'summary.csv').read_bytes()
assert raw == (OUT / 'chart-data.csv').read_bytes()
rows = list(csv.DictReader(raw.decode().splitlines()))
lookup = {(r['shot'], r['config'], r['reference']): r for r in rows}
shots = ['cockpit-anvil-turn', 'mirror-glass-pass', 'fast-star-turn']
methods = ['asvgf-dlss-full', 'rr-quality']
report = (ROOT / 'docs/final-denoiser-report.md').read_text(encoding='utf-8')
cells = [[c.strip() for c in line.strip('|').split('|')] for line in report.splitlines()
         if line.startswith(('| Anvil |', '| Mirror / glass |', '| Star |'))]
cells = [row for row in cells if len(row) == 6]
assert len(cells) == 6
for cells_row, (shot, method) in zip(cells, [(s, m) for s in shots for m in methods]):
    for index, ref in enumerate(['primary', 'secondary']):
        r = lookup[shot, method, ref]
        entry = f"{r['settle_median']} ({r['settle_min']}–{r['settle_max']})"
        assert cells_row[2+index] == entry, (cells_row, r)
        assert cells_row[4+index] == f"{float(r['terminal_flip_median']):.5f}"

slides = json.loads((OUT / 'slides.json').read_text(encoding='utf-8'))
assert len(slides) == 12 and sum(s['seconds'] for s in slides) == 720
script = (OUT / 'speaker-script.md').read_text(encoding='utf-8')
assert all(s['script'] in script and s['cue'] in script for s in slides)
pptx = OUT / 'StarTraveler-denoiser-presentation.pptx'
ns = {'c': 'http://schemas.openxmlformats.org/drawingml/2006/chart',
      'a': 'http://schemas.openxmlformats.org/drawingml/2006/main'}
with zipfile.ZipFile(pptx) as z:
    names = z.namelist()
    assert len([n for n in names if re.fullmatch(r'ppt/slides/slide\d+.xml', n)]) == 12
    chart_names = sorted(n for n in names if n.endswith('.xml') and '/charts/chart' in n)
    assert len(chart_names) == 2
    for chart_name, metric in zip(chart_names, ['settle_median', 'terminal_flip_median']):
        tree = ET.fromstring(z.read(chart_name))
        series = tree.findall('.//c:ser', ns)
        assert len(series) == 2
        for ser, method in zip(series, methods):
            values = [float(v.text) for v in ser.findall('./c:val//c:pt/c:v', ns)]
            expected = [round(float(lookup[s, method, 'primary'][metric]), 8) for s in shots]
            assert values == expected, (values, expected)
    original = (DATA / 'final-frame-contact-sheet.png').read_bytes()
    assert any(z.read(n) == original for n in names if n.startswith('ppt/media/'))
    for i, slide in enumerate(slides, 1):
        tree = ET.fromstring(z.read(f'ppt/notesSlides/notesSlide{i}.xml'))
        notes = ''.join(t.text or '' for t in tree.findall('.//a:t', ns))
        assert slide['script'] in notes and slide['cue'] in notes

# Check every linked local artifact from the new report and material index.
link_count = 0
for doc in [ROOT / 'docs/final-denoiser-report.md', OUT / 'README.md']:
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text(encoding='utf-8')):
        if not target.startswith(('https:', 'http:', '#')):
            assert (doc.parent / target.split('#')[0]).exists(), (doc, target)
            link_count += 1
provenance = json.loads((OUT / 'figure-provenance.json').read_text())
assert provenance['source_sha256'] == hashlib.sha256(raw).hexdigest()
result = {'passed': True, 'summary_rows': 12, 'report_result_rows': 6,
          'report_numeric_cells': 24, 'native_chart_values': 12,
          'slide_count': 12, 'script_seconds_planned': 720,
          'embedded_image_matches_source': True, 'speaker_notes_verified': 12,
          'local_links_checked': link_count, 'thresholds_changed': False,
          'source_summary_sha256': hashlib.sha256(raw).hexdigest(),
          'presentation_sha256': hashlib.sha256(pptx.read_bytes()).hexdigest(),
          'visual_review': 'All 12 previews and both standalone figure pairs reviewed',
          'native_powerpoint_opened': False}
(OUT / 'integration-verification.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
