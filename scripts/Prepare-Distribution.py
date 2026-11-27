"""Prepare a fresh, dated distribution from the current production build."""
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='0.2.2')
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--rtx50-runtime', type=Path,
                        help='Optional NVIDIA original runtime included in the same main package')
    parser.add_argument('--validation-report', type=Path,
                        help='Validation summary paired with the current production DLL and default runtime')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.version):
        parser.error('Version must be a single directory name')
    project = Path(__file__).resolve().parents[1]
    stage = project / 'dist' / args.version
    if stage.exists():
        raise SystemExit(f'Refusing to merge with an existing distribution: {stage}')
    build_package = project / 'build/package/obs-dlss5'
    plugin = build_package / 'bin/64bit/obs-dlss5.dll'
    release = project / 'build/Release/obs-dlss5.dll'
    if digest(plugin) != digest(release):
        raise SystemExit('Staged plugin differs from the Release build')
    for file in (project / 'data').rglob('*'):
        if file.is_file() and digest(file) != digest(build_package / 'data' / file.relative_to(project / 'data')):
            raise SystemExit(f'Stale staged resource: {file.name}; rebuild before packaging')
    runtime = args.runtime or build_package / 'bin/64bit/runtime/nvngx_dlssnr.dll'
    if not runtime.is_file():
        raise SystemExit('A bundled DLSSNR runtime is required for this distribution')
    report = {}
    if args.validation_report:
        report = json.loads(args.validation_report.read_text(encoding='utf-8-sig'))
        if report.get('PluginSHA256', '').upper() != digest(release):
            raise SystemExit('Validation report is not paired with the current production DLL')
        if report.get('RuntimeSHA256', '').upper() != digest(runtime):
            raise SystemExit('Validation report is not paired with the selected default runtime')
    if args.rtx50_runtime and not args.rtx50_runtime.is_file():
        raise SystemExit('The optional RTX 50 runtime does not exist')
    stage.mkdir(parents=True)
    shutil.copytree(build_package, stage / 'obs-dlss5')
    target_runtime = stage / 'obs-dlss5/bin/64bit/runtime/nvngx_dlssnr.dll'
    target_runtime.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime, target_runtime)
    runtime_options = []
    if args.rtx50_runtime:
        option = stage / 'obs-dlss5/bin/64bit/runtime/rtx50/nvngx_dlssnr.dll'
        option.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.rtx50_runtime, option)
        runtime_options.append({'File': 'bin/64bit/runtime/rtx50/nvngx_dlssnr.dll',
                                'SHA256': digest(option), 'Bytes': option.stat().st_size,
                                'Selection': 'Manual; NVIDIA original RTX 50 option'})
    shutil.copy2(project / 'docs/DISTRIBUTION-INSTALL.md', stage / '安装说明.txt')
    shutil.copy2(project / 'scripts/Install.cmd', stage / 'Install.cmd')
    shutil.copy2(project / 'scripts/Uninstall.cmd', stage / 'Uninstall.cmd')
    (stage / 'installer').mkdir(parents=True, exist_ok=True)
    shutil.copy2(project / 'scripts/Manage-Install.ps1', stage / 'installer/Manage-Install.ps1')
    for name in ('THIRD_PARTY_NOTICES.md', 'LICENSE'):
        shutil.copy2(project / name, stage / 'obs-dlss5/data' / name)
    files = []
    for file in sorted((stage / 'obs-dlss5').rglob('*')):
        if file.is_file():
            if file.suffix.lower() in ('.exe', '.pdb', '.lib', '.exp') or 'identity' in file.name.lower():
                raise SystemExit(f'Test/build artifact in production payload: {file}')
            files.append({'Relative': str(file.relative_to(stage / 'obs-dlss5')).replace('/', '\\'),
                          'Bytes': file.stat().st_size, 'SHA256': digest(file)})
    manifest = {
        'Plugin': 'DLSS5 Neural Filter', 'Version': args.version,
        'PluginVersion': re.search(r'project\(Dlss5NeuralFilter VERSION ([0-9.]+)',
                                  (project / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1),
        'Revision': 'ui-polish-and-magpie-0614-sync',
        'License': 'GPL-3.0-only',
        'SourceRepository': 'https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter',
        'OBSVersionHardGate': False, 'HostVersionListIsAllowlist': False,
        'Admission': 'required API exports + D3D11/D3D12 capabilities + Create/Evaluate',
        'ImplementedRequirements': list(range(1, 19)) + [23], 'RuntimeIncluded': True,
        'Runtime': {'File': 'bin/64bit/runtime/nvngx_dlssnr.dll',
                    'SHA256': digest(target_runtime), 'Bytes': target_runtime.stat().st_size,
                    'IdentityIsAllowlist': False},
        'RuntimeOptions': runtime_options,
        'ValidatedHostVersions': report.get('ValidatedHostVersions', []),
        'ValidatedGPU': report.get('ValidatedGPU', ''),
        'Validation': report.get('Validation', {'Status': 'No paired validation report supplied'}),
        'PendingAcceptance': report.get('PendingAcceptance', []),
        'InstallGuide': '安装说明.txt',
        'InstallerFiles': ['Install.cmd', 'Uninstall.cmd', 'installer/Manage-Install.ps1'],
        'Files': files,
    }
    (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps({'distribution': str(stage), 'pluginSha256': digest(plugin),
                      'payloadFiles': len(files)}, indent=2))


if __name__ == '__main__':
    main()
