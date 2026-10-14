"""Archive a verified distribution and pair it with the current project source."""
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import zipfile


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest().upper()


def record(file):
    return {'bytes': file.stat().st_size, 'sha256': digest(file)}


def source_files(project):
    roots = ('.gitignore', 'CMakeLists.txt', 'LICENSE', 'README.md', 'README_EN.md', 'RELEASE-NOTES.md', 'THIRD_PARTY_NOTICES.md')
    files = [project / name for name in roots]
    files.extend(project / 'docs' / name for name in ('BUILDING.md', 'DISTRIBUTION-INSTALL.md'))
    for folder in ('src', 'data', 'cmake', 'scripts', 'tests', 'licenses'):
        for file in (project / folder).rglob('*'):
            if file.is_file() and 'results' not in file.relative_to(project).parts and '__pycache__' not in file.parts:
                if file.relative_to(project).as_posix() == 'tests/PrepareObsAcceptance.py':
                    continue  # Local GUI fixture helper reads the author's scene collection.
                if file.suffix.lower() in ('.dll', '.exe', '.pdb', '.lib', '.exp', '.pyc'):
                    raise SystemExit(f'Binary artifact in source set: {file}')
                files.append(file)
    return sorted(files)


def assert_zip(path, forbidden):
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise SystemExit(f'ZIP CRC failure: {path}')
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise SystemExit(f'Duplicate ZIP names: {path}')
        for name in archive.namelist():
            if name.startswith('/') or '..' in Path(name).parts or forbidden(name):
                raise SystemExit(f'Unexpected ZIP member: {name}')


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=Path('dist/0.1.0'))
    parser.add_argument('--output-stem', default='DLSS5-Neural-Filter-0.1.0')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.output_stem):
        parser.error('Output stem must be a single file name')
    candidate = args.candidate if args.candidate.is_absolute() else project / args.candidate
    candidate = candidate.resolve()
    if not candidate.is_relative_to((project / 'dist').resolve()):
        raise SystemExit('Distribution must be inside project/dist')
    manifest_path = candidate / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if manifest['Plugin'] != 'DLSS5 Neural Filter' or manifest['OBSVersionHardGate'] is not False or manifest['HostVersionListIsAllowlist'] is not False:
        raise SystemExit('Unexpected plugin metadata or version restriction')
    payload = candidate / 'obs-dlss5'
    declared = set()
    for entry in manifest['Files']:
        file = (payload / entry['Relative'].replace('\\', '/')).resolve()
        if not file.is_relative_to(payload.resolve()) or file in declared:
            raise SystemExit('Manifest path escaped package or is duplicated')
        if digest(file) != entry['SHA256']:
            raise SystemExit(f'Package checksum mismatch: {file}')
        declared.add(file)
    actual = {file.resolve() for file in payload.rglob('*') if file.is_file()}
    if declared != actual:
        raise SystemExit('Manifest and actual payload file sets differ')
    plugin = payload / 'bin/64bit/obs-dlss5.dll'
    if digest(plugin) != digest(project / 'build/Release/obs-dlss5.dll'):
        raise SystemExit('Candidate DLL differs from current production build; refusing mismatched source pairing')
    inputs = [project / 'CMakeLists.txt'] + [file for folder in ('src', 'cmake') for file in (project / folder).rglob('*') if file.is_file()]
    inputs.append(project / 'data/neural.hlsl')
    if any(file.stat().st_mtime > plugin.stat().st_mtime for file in inputs):
        raise SystemExit('Production source inputs changed after build; rebuild before packaging')
    for file in (project / 'data').rglob('*'):
        if file.is_file() and digest(file) != digest(payload / 'data' / file.relative_to(project / 'data')):
            raise SystemExit(f'Candidate resource differs from source: {file.name}')

    source_zip = project / 'dist' / (args.output_stem + '-source.zip')
    sources = source_files(project)
    source_manifest = {
        'Plugin': manifest['Plugin'], 'DistributionVersion': manifest['Version'],
        'Revision': manifest['Revision'], 'PairedProductionDllSHA256': digest(plugin),
        'Files': [{'Relative': file.relative_to(project).as_posix(), 'SHA256': digest(file)} for file in sources],
        'ExternalDependencies': 'obs/docs/BUILDING.md',
    }
    if (project / '.git').exists():
        source_commit = subprocess.check_output(['git', '-C', str(project), 'rev-parse', 'HEAD'], text=True).strip()
        for file in sources:
            relative = file.relative_to(project).as_posix()
            committed = subprocess.check_output(['git', '-C', str(project), 'show', f'HEAD:{relative}'])
            if hashlib.sha256(committed).hexdigest().upper() != digest(file):
                raise SystemExit(f'Source differs from the selected commit: {relative}')
        source_manifest['SourceCommit'] = source_commit
        manifest['SourceCommit'] = source_commit
    with zipfile.ZipFile(source_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in sources:
            archive.write(file, 'obs/' + file.relative_to(project).as_posix())
        archive.writestr('source-manifest.json', json.dumps(source_manifest, indent=2, ensure_ascii=False) + '\n')
    assert_zip(source_zip, lambda name: name.lower().endswith(('.dll', '.exe', '.pdb', '.lib', '.exp', '.pyc')) or '/results/' in name or '/.deps/' in name)
    manifest['SourceArchive'] = {'Name': source_zip.name, **record(source_zip)}
    manifest['PackageFiles'] = [{'Relative': file.relative_to(candidate).as_posix(), 'SHA256': digest(file)}
                               for file in sorted(candidate.rglob('*')) if file.is_file() and file != manifest_path]
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

    binary_zip = project / 'dist' / (args.output_stem + '-Windows-x64.zip')
    with zipfile.ZipFile(binary_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in sorted(candidate.rglob('*')):
            if file.is_file():
                archive.write(file, file.relative_to(candidate).as_posix())
    assert_zip(binary_zip, lambda name: 'identity' in name.lower() or name.lower().endswith(('.exe', '.pdb', '.lib', '.exp')) or '/config/' in name or '/logs/' in name)
    with zipfile.ZipFile(binary_zip) as archive:
        for entry in manifest['PackageFiles']:
            actual_hash = hashlib.sha256(archive.read(entry['Relative'])).hexdigest().upper()
            if actual_hash != entry['SHA256']:
                raise SystemExit(f'Archived package file differs: {entry["Relative"]}')
    records = {file.name: record(file) for file in (binary_zip, source_zip)}
    (project / 'dist' / (args.output_stem + '-checksums.json')).write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    (project / 'dist' / (args.output_stem + '-SHA256.txt')).write_text(''.join(f'{entry["sha256"]}  {name}\n' for name, entry in records.items()), encoding='utf-8')
    print(json.dumps(records, indent=2))


if __name__ == '__main__':
    main()
