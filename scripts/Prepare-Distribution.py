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
    parser.add_argument('--version', default='0.1.0')
    parser.add_argument('--runtime', type=Path)
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
    stage.mkdir(parents=True)
    shutil.copytree(build_package, stage / 'obs-dlss5')
    target_runtime = stage / 'obs-dlss5/bin/64bit/runtime/nvngx_dlssnr.dll'
    target_runtime.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime, target_runtime)
    for name in ('Install.ps1', 'Install.cmd', 'Install-OBS.ps1', 'Install-OBS.cmd',
                 'Deploy-SteamLocal.ps1', 'Restore.ps1', 'Delivery.ps1', 'Verify-Package.ps1'):
        shutil.copy2(project / 'scripts' / name, stage / name)
    shutil.copy2(project / 'docs/DISTRIBUTION-INSTALL.md', stage / 'INSTALL.md')
    for name in ('README.md', 'README_EN.md', 'RELEASE-NOTES.md'):
        shutil.copy2(project / name, stage / name)
    (stage / 'docs').mkdir()
    for name in ('BUILDING.md', 'DISTRIBUTION-INSTALL.md'):
        shutil.copy2(project / 'docs' / name, stage / 'docs' / name)
    shutil.copy2(project / 'THIRD_PARTY_NOTICES.md', stage / 'THIRD_PARTY_NOTICES.md')
    shutil.copy2(project / 'LICENSE', stage / 'LICENSE')
    shutil.copytree(project / 'licenses', stage / 'licenses')
    files = []
    for file in sorted((stage / 'obs-dlss5').rglob('*')):
        if file.is_file():
            if file.suffix.lower() in ('.exe', '.pdb', '.lib', '.exp') or 'identity' in file.name.lower():
                raise SystemExit(f'Test/build artifact in production payload: {file}')
            files.append({'Relative': str(file.relative_to(stage / 'obs-dlss5')).replace('/', '\\'),
                          'Bytes': file.stat().st_size, 'SHA256': digest(file)})
    manifest = {
        'Plugin': 'DLSS5 Neural Filter', 'Version': args.version, 'PluginVersion': '0.1.0',
        'Revision': '20261010-completed-preview-stability',
        'License': 'GPL-3.0-only',
        'SourceRepository': 'https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter',
        'OBSVersionHardGate': False, 'HostVersionListIsAllowlist': False,
        'Admission': 'required API exports + D3D11/D3D12 capabilities + Create/Evaluate',
        'ImplementedRequirements': list(range(1, 19)) + [23], 'RuntimeIncluded': True,
        'Runtime': {'File': 'bin/64bit/runtime/nvngx_dlssnr.dll',
                    'SHA256': digest(target_runtime), 'Bytes': target_runtime.stat().st_size,
                    'IdentityIsAllowlist': False},
        'ValidatedHostVersions': ['32.2.2'], 'ValidatedGPU': 'RTX 5070 Ti / 616.92',
        'Validation': {'VerifiedBuildDate': '2026-10-10', 'ContractTests': '8/8 passed',
                       'ProductionLibobsNeuralAlphaAndRecovery': 'passed',
                       'OffscreenDualPreviewPairs': 90,
                       'CompleteNativeDialogAcceptance': False,
                       'LongDurationAcceptance': False},
        'PendingAcceptance': ['complete native filter-dialog UI/DPI combinations',
                              'live game/camera sources', 'other GPUs',
                              '30-minute OBS statistics', '2-hour sustained processing',
                              'end-to-end latency', 'real-content AF artifacts'],
        'InstallGuide': 'INSTALL.md', 'ReleaseNotes': 'RELEASE-NOTES.md', 'Files': files,
    }
    (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps({'distribution': str(stage), 'pluginSha256': digest(plugin),
                      'payloadFiles': len(files)}, indent=2))


if __name__ == '__main__':
    main()
