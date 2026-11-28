"""Create allowlisted local team archives without changing scientific captures."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[3]
STAGE = ROOT / '.handoff-build/StarTraveler-team'
OUT = ROOT / 'handoff/2026-11-27'
TAG = '2026-11-27'
BIN = Path('build/msvc-vs2022/bin/Release')
EXCLUDED = {'__pycache__', '.pytest_cache', '.git', 'node_modules', '.venv', 'build'}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def copy(source, relative=None):
    assert source.is_file() and not source.is_symlink(), source
    target = STAGE / (relative or source.relative_to(ROOT))
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def prepare():
    if STAGE.exists():
        raise ValueError('Staging already exists; choose new staging/output paths')
    STAGE.mkdir(parents=True)
    for name in ['StarCore', 'StarTraveler', 'docs']:
        for p in (ROOT / name).rglob('*'):
            if p.is_file() and not any(part in EXCLUDED for part in p.relative_to(ROOT/name).parts):
                if p.suffix not in {'.pyc', '.pyo', '.pyd'}:
                    copy(p)
    for name in ['CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'README.md', 'TODO.md',
                 'AGENTS.md', '.gitignore', '.gitattributes', '.clang-format', '.clangd',
                 'HANDOFF.md', 'OPEN_ISSUES.md', 'VERIFY_PACKAGE.py']:
        copy(ROOT / name)
    for folder in ['production-lag-fixed-exposure', 'formal-ac', 'measurement',
                   'fixed-exposure-validation', 'gpu-regression', 'quality-fixed-exposure']:
        for p in (ROOT / 'renders' / folder).iterdir():
            if not p.is_file() or p.name.startswith(('still_', 'moving_', 'frame_')):
                continue
            if p.suffix in {'.json', '.csv'} or (folder in
                    {'production-lag-fixed-exposure', 'formal-ac', 'quality-fixed-exposure'} and p.suffix == '.png'):
                copy(p)
    for p in (ROOT / 'renders/window-rehearsal/backup-fixed-exposure').iterdir():
        if p.is_file() and p.suffix in {'.json', '.csv', '.png', '.jpg', '.mp4', '.log'}:
            copy(p)
    copy(ROOT / 'renders/window-rehearsal/fixed-exposure-desktop-calibration/negative-control-checks.json')
    for p in (ROOT / 'renders/demo-presets').rglob('*'):
        if p.is_file() and p.suffix in {'.json', '.png', '.jpg', '.log'}:
            copy(p)
    for p in (ROOT / BIN).rglob('*'):
        if p.is_file() and (p.suffix in {'.exe', '.dll'} or 'assets' in p.parts):
            copy(p)
    vendor = ROOT / 'build/msvc-vs2022/_deps/upscalers'
    for name in ['nvngx_dlss.dll', 'nvngx_dlssd.dll']:
        copy(vendor / 'dlss-sdk-310.9.1/lib/Windows_x86_64/rel' / name, BIN / name)
    copy(vendor / 'fidelityfx-sdk-1.1.4/PrebuiltSignedDLL/amd_fidelityfx_vk.dll',
         BIN / 'amd_fidelityfx_vk.dll')
    for sdk in ['dlss-sdk-310.9.1', 'fidelityfx-sdk-1.1.4']:
        copy(vendor / sdk / 'LICENSE.txt', Path('THIRD-PARTY-NOTICES') / sdk / 'LICENSE.txt')
    for p in (ROOT / 'build/msvc-vs2022/vcpkg_installed/x64-windows-static-md/share').rglob('copyright'):
        copy(p, Path('THIRD-PARTY-NOTICES/vcpkg') / p.parent.name / 'copyright')
    for package in ['vulkan-loader', 'vulkan-headers']:
        p = ROOT.parent / '.build-tools/vcpkg/packages' / (package + '_x64-windows-static-md') / 'share' / package / 'copyright'
        copy(p, Path('THIRD-PARTY-NOTICES/vcpkg') / package / 'copyright')
    commands = {
        '01-Play.cmd': '"build\\msvc-vs2022\\bin\\Release\\StarTraveler.exe" --upscaler DLSS --quality Quality --dlss-preset K --dlss-exposure 1 --no-viewports',
        '02-Fixed-Exposure-Flight.cmd': '"build\\msvc-vs2022\\bin\\Release\\StarTraveler.exe" --upscaler DLSS --quality Quality --dlss-preset K --dlss-exposure 1 --no-viewports --rehearsal-seconds 20',
        '03-Open-Backup.cmd': 'start "" "renders\\window-rehearsal\\backup-fixed-exposure\\flight.mp4"',
    }
    for name, command in commands.items():
        (STAGE / name).write_bytes(('@echo off\r\ncd /d "%~dp0"\r\n' + command + '\r\n').encode('ascii'))
    print(json.dumps({'prepared': str(STAGE), 'files': sum(p.is_file() for p in STAGE.rglob('*'))}))


def pack():
    OUT.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in STAGE.rglob('*') if p.is_file())
    assert files and (STAGE / 'handoff-validation/relocation.json').exists()
    assert json.loads((STAGE / 'handoff-validation/relocation.json').read_text())['passed']
    results = []
    for mode in ['source', 'full']:
        selected = [p for p in files if mode == 'full' or
                    (not p.relative_to(STAGE).as_posix().startswith(('build/', 'THIRD-PARTY-NOTICES/', 'handoff-validation/'))
                     and p.suffix.lower() not in {'.exe', '.dll', '.mp4', '.cmd'}
                     and not p.name.startswith('motion-'))]
        entries = {p.relative_to(STAGE).as_posix(): {'bytes': p.stat().st_size, 'sha256': digest(p)}
                   for p in selected}
        manifest = {'kind': mode, 'date': '2026-11-27', 'files': entries,
                    'scientific_raw_frame_archive_included': False,
                    'historical_absolute_paths_preserved': True,
                    'no_git_history': True, 'not_a_complete_cmake_build_tree': True}
        target = OUT / f'StarTraveler-team-{mode}-{TAG}.zip'
        with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=5) as z:
            for p in selected:
                if (STAGE.parent / 'pause.request').exists():
                    raise RuntimeError('Handoff packaging paused by supervising agent')
                z.write(p, 'StarTraveler-team/' + p.relative_to(STAGE).as_posix())
            z.writestr(f'StarTraveler-team/PACKAGE-MANIFEST-{mode}.json', json.dumps(manifest, indent=2))
        with zipfile.ZipFile(target) as z:
            assert z.testzip() is None
            for relative, item in entries.items():
                actual = z.read('StarTraveler-team/' + relative)
                assert len(actual) == item['bytes']
                assert hashlib.sha256(actual).hexdigest() == item['sha256']
        results.append({'file': target.name, 'bytes': target.stat().st_size,
                        'sha256': digest(target), 'files': len(entries),
                        'zip_crc_and_all_file_hashes_passed': True})
        print(json.dumps(results[-1]), flush=True)
    (OUT / 'SHA256SUMS.txt').write_text(''.join(f"{r['sha256']}  {r['file']}\n" for r in results))
    (OUT / 'package-verification.json').write_text(json.dumps({'passed': True, 'archives': results}, indent=2))
    for name in ['HANDOFF.md', 'OPEN_ISSUES.md']:
        shutil.copy2(ROOT / name, OUT / name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'pack'])
    parser.add_argument('--tag', default='2026-11-27')
    args = parser.parse_args()
    if not args.tag or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in args.tag):
        raise ValueError('Tag must contain lowercase letters, digits and hyphens')
    TAG = args.tag
    if TAG != '2026-11-27':
        STAGE = ROOT / '.handoff-build' / TAG / 'StarTraveler-team'
        OUT = ROOT / 'handoff' / TAG
    prepare() if args.action == 'prepare' else pack()
