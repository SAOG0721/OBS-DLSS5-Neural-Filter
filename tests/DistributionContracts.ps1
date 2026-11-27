# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [string]$ZipPath=''
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$resolvedPkg=[IO.Path]::GetFullPath($PackageRoot)
$manifest=Get-Content -LiteralPath (Join-Path $resolvedPkg 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (!$ZipPath) {
    $ZipPath=Join-Path $project ("dist\DLSS5-Neural-Filter-$($manifest.Version)-Windows-x64.zip")
}
if (!(Test-Path -LiteralPath $ZipPath -PathType Leaf)) { throw "Release ZIP not found: $ZipPath" }

$verification=& (Join-Path $project 'scripts\Verify-Package.ps1') -PackageRoot $resolvedPkg
if ($verification.State -ne 'Verified') { throw 'Package verification failed' }

$fixture=Join-Path $project ('tests\results\distribution-'+[Guid]::NewGuid().ToString('N'))
$extracted=Join-Path $fixture 'extracted'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $extracted)

foreach ($expectedTop in @('Install.cmd', 'Uninstall.cmd', 'installer\Manage-Install.ps1', '安装说明.txt', 'obs-plugins\64bit\obs-dlss5.dll', 'obs-plugins\64bit\runtime\nvngx_dlssnr.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $extracted $expectedTop) -PathType Leaf)) {
        throw "Extracted release ZIP is missing expected entry: $expectedTop"
    }
}

$target=Join-Path $fixture 'OBS Application'
$backup=Join-Path $fixture 'backups'
$legacyDir=Join-Path $fixture 'legacy-programdata\obs-dlss5'
$exe=Join-Path $target 'bin\64bit\obs64.exe'
$oldPlugin=Join-Path $target 'obs-plugins\64bit\obs-dlss5.dll'
$legacyDll=Join-Path $legacyDir 'bin\64bit\obs-dlss5.dll'
foreach ($file in @($exe, $oldPlugin, $legacyDll)) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file)) -Force | Out-Null
    [IO.File]::WriteAllText($file, 'existing application fixture')
}
$oldHash=(Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash
$sentinel=Join-Path $target 'user-note.txt'
[IO.File]::WriteAllText($sentinel, 'preserve unrelated files')
$unrelated=Join-Path $target 'unrelated-empty-directory'
New-Item -ItemType Directory -Path $unrelated | Out-Null

$manageScript=Join-Path $extracted 'installer\Manage-Install.ps1'

# 1. Preview must not touch disk or create backups
$previewJson=& $manageScript -Mode Install -ObsRoot $target -BackupRoot $backup -LegacyProgramDataDir $legacyDir -NonInteractive
$preview=$previewJson | ConvertFrom-Json
if ($preview.Mode -ne 'Preview' -or $preview.Files.Count -ne $manifest.Files.Count) {
    throw 'Installer preview output mismatch'
}
if ((Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash -ne $oldHash -or (Test-Path -LiteralPath $backup)) {
    throw 'Preview modified target or created a backup'
}

# 2. Apply install using obs64.exe path directly (verifies smart parent resolution + legacy ProgramData migration)
$installed=& $manageScript -Mode Install -ObsRoot $exe -BackupRoot $backup -LegacyProgramDataDir $legacyDir -CleanLegacyProgramData -NonInteractive -Apply
if ($installed.State -ne 'Installed' -or $installed.FileCount -ne $manifest.Files.Count) {
    throw 'One-click installer apply failed'
}
if (Test-Path -LiteralPath $legacyDir) { throw 'Legacy ProgramData directory was not moved out' }
if (!$installed.LegacyMoved -or !(Test-Path -LiteralPath (Join-Path $installed.LegacyMoved 'bin\64bit\obs-dlss5.dll'))) {
    throw 'Legacy ProgramData backup is missing'
}
$receipt=Get-Content -LiteralPath $installed.Receipt -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $receipt.Entries) {
    $file=Join-Path $target $entry.Relative
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.InstalledSHA256) {
        throw "Installed hash mismatch: $($entry.Relative)"
    }
}

# 3. Restore via Restore.ps1
$restored=& (Join-Path $project 'scripts\Restore.ps1') -Receipt $installed.Receipt -Apply
if ($restored.State -ne 'Restored' -or (Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash -ne $oldHash) {
    throw 'Rollback did not restore prior plugin'
}
if (!(Test-Path -LiteralPath $sentinel) -or !(Test-Path -LiteralPath $unrelated) -or
    @(Get-ChildItem -LiteralPath $target -Recurse -File).Count -ne 3) {
    throw 'Rollback altered unrelated OBS contents'
}

# 4. Re-install clean, then test Uninstall leaves unrelated files and dirs untouched
$reinstalled=& $manageScript -Mode Install -ObsRoot $target -BackupRoot $backup -LegacyProgramDataDir (Join-Path $fixture 'no-legacy') -NonInteractive -Apply
if ($reinstalled.State -ne 'Installed') { throw 'Re-install failed' }
$uninstalled=& $manageScript -Mode Uninstall -ObsRoot $target -LegacyProgramDataDir (Join-Path $fixture 'no-legacy') -NonInteractive -Apply
if ($uninstalled.State -ne 'Uninstalled' -or $uninstalled.RemovedFiles -ne $manifest.Files.Count) {
    throw 'Uninstall did not remove expected plugin files'
}
if (Test-Path -LiteralPath $oldPlugin) { throw 'Plugin DLL remained after uninstall' }
if (Test-Path -LiteralPath (Join-Path $target 'data\obs-plugins\obs-dlss5')) { throw 'Plugin data directory remained after uninstall' }
if (!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath $sentinel) -or !(Test-Path -LiteralPath $unrelated)) {
    throw 'Uninstall removed unrelated OBS files or directories'
}

# 5. Damaged package rejection
$badExtracted=Join-Path $fixture 'damaged-extracted'
Copy-Item -LiteralPath $extracted -Destination $badExtracted -Recurse
Remove-Item -LiteralPath (Join-Path $badExtracted 'obs-plugins\64bit\obs-dlss5.dll') -Force
$rejected=$false
try {
    & (Join-Path $badExtracted 'installer\Manage-Install.ps1') -Mode Install -ObsRoot $target -BackupRoot $backup -NonInteractive | Out-Null
} catch {
    $rejected=$true
}
if (!$rejected) { throw 'Damaged extracted package was accepted' }

[pscustomobject]@{
    State='Passed'
    PowerShell=$PSVersionTable.PSVersion.ToString()
    Coverage='zip extraction; preview; obs64 parent resolution; install; legacy ProgramData cleanup; rollback; clean uninstall; damaged package rejection'
    Fixture=$fixture
}
