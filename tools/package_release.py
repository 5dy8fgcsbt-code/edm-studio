"""Package only application sources and the built runtime; never game assets."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist' / 'EDM-Studio'
RELEASE = ROOT / 'release'


def archive(path, entries):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for source, target in entries:
            if source.is_file():
                z.write(source, target)
            else:
                for item in sorted(source.rglob('*')):
                    if item.is_file() and '__pycache__' not in item.parts and item.suffix != '.pyc':
                        z.write(item, str(Path(target) / item.relative_to(source)))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist',type=Path,default=DIST,help='Built EDM-Studio directory to package')
    opts=parser.parse_args();dist=opts.dist.resolve()
    if not (dist / 'EDM-Studio.exe').is_file():
        raise SystemExit('Build EDM-Studio.exe first.')
    if (dist / '_internal' / 'icuuc.dll').exists():
        raise SystemExit('Unexpected bundled ICU DLL. Rebuild with the sanitized PATH in EDM-Studio.spec.')
    RELEASE.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'README.md', dist / '使用说明.md')
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', dist / 'THIRD_PARTY_NOTICES.md')
    for name in ('docs', 'samples', 'licenses'):
        shutil.copytree(ROOT / name, dist / name, dirs_exist_ok=True)
    evidence = [p for p in (ROOT / 'validation').glob('*.json')
                if p.name in ('native_animation_results.json', 'blender_import_result.json',
                              'ui_smoke.json', 'frozen_smoke.json', 'frozen_livery.json', 'livery_validation.json',
                              'frozen_materials.json','materials_validation.json','blender-pbr-numbers.json',
                              'f14-wing-fix.json','frozen_wing_fix.json','c130-source.json','c130-frozen.json',
                              'overlay-ui.json','overlay-frozen.json','f100-source.json','f100-frozen.json')
                or p.name.endswith(('.validation.json', '.roundtrip.json'))]
    (dist / 'validation').mkdir(exist_ok=True)
    for p in evidence:
        shutil.copy2(p, dist / 'validation' / p.name)
    binary = RELEASE / 'EDM-Studio-Windows-x64.zip'
    archive(binary, [(dist, 'EDM-Studio')])
    sources = ['main.py', 'edm_studio', 'tests', 'samples', 'docs', 'licenses',
               'README.md', 'THIRD_PARTY_NOTICES.md', 'requirements.txt',
               'requirements-dev.txt', 'build.ps1', 'EDM-Studio.spec']
    entries = [(ROOT / name, 'edm-studio/' + name) for name in sources]
    # The tools directory can contain installed research dependencies; include only scripts.
    entries += [(p, 'edm-studio/tools/' + p.name) for p in (ROOT / 'tools').iterdir()
                if p.is_file() and p.suffix in ('.py', '.cjs')]
    entries += [(ROOT / 'tools' / 'gltf-check' / name, 'edm-studio/tools/gltf-check/' + name)
                for name in ('package.json','package-lock.json')]
    entries += [(p, 'edm-studio/validation/' + p.name) for p in evidence]
    source = RELEASE / 'EDM-Studio-Source.zip'
    archive(source, entries)
    checksums = {p.name: {'bytes': p.stat().st_size,
                          'sha256': hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()}
                 for p in (binary, source)}
    (RELEASE / 'SHA256.json').write_text(json.dumps(checksums, indent=2), encoding='utf-8')
    print(json.dumps(checksums, indent=2))


if __name__ == '__main__':
    main()
