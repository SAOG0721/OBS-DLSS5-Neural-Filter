# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture=Join-Path $project ('tests\results\distribution-'+[Guid]::NewGuid().ToString('N'))
$target=Join-Path $fixture 'OBS Application'
$backup=Join-Path $fixture 'backups'
$exe=Join-Path $target 'bin\64bit\obs64.exe'
$oldPlugin=Join-Path $target 'obs-plugins\64bit\obs-dlss5.dll'
foreach ($file in @($exe,$oldPlugin)) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file)) -Force | Out-Null
    [IO.File]::WriteAllText($file,'existing application fixture')
}
$oldHash=(Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash
$sentinel=Join-Path $target 'user-note.txt'
[IO.File]::WriteAllText($sentinel,'preserve unrelated files')
$unrelated=Join-Path $target 'unrelated-empty-directory'
New-Item -ItemType Directory -Path $unrelated | Out-Null
$verification=& (Join-Path $PackageRoot 'Verify-Package.ps1') -PackageRoot $PackageRoot
if ($verification.State -ne 'Verified') { throw 'Package verification failed' }
$runner=if ($PSVersionTable.PSVersion.Major -ge 6) { Join-Path $PSHOME 'pwsh.exe' } else { Join-Path $PSHOME 'powershell.exe' }
# Use -File without PackageRoot, exactly as the double-click entry points do.
& $runner -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageRoot 'Verify-Package.ps1') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Native -File verification/default package root failed' }
& $runner -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageRoot 'Install.ps1') -Destination (Join-Path $fixture 'default-preview') -BackupRoot (Join-Path $fixture 'default-backup') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Native -File standard preview/default package root failed' }
& $runner -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageRoot 'Install-OBS.ps1') -ObsRoot $target -BackupRoot (Join-Path $fixture 'default-backup') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Native -File application preview/default package root failed' }
if ((Test-Path -LiteralPath (Join-Path $fixture 'default-preview')) -or
    (Test-Path -LiteralPath (Join-Path $fixture 'default-backup'))) { throw 'Native default preview wrote files' }
$preview=& (Join-Path $PackageRoot 'Install-OBS.ps1') -ObsRoot $target -PackageRoot $PackageRoot -BackupRoot $backup
if ((Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash -ne $oldHash -or (Test-Path -LiteralPath $backup)) {
    throw 'Application-local preview modified files'
}
$installed=& (Join-Path $PackageRoot 'Install-OBS.ps1') -ObsRoot $target -PackageRoot $PackageRoot -BackupRoot $backup -Apply
if ($installed.State -ne 'Installed') { throw 'Application-local installation failed' }
$receipt=Get-Content -LiteralPath $installed.Receipt -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $receipt.Entries) {
    if ((Get-FileHash -LiteralPath (Join-Path $target $entry.Relative) -Algorithm SHA256).Hash -ne $entry.InstalledSHA256) {
        throw 'Application-local installed hash differs from receipt'
    }
}
$restored=& (Join-Path $receipt.BackupDirectory 'Restore.ps1') -Receipt $installed.Receipt -Apply
if ($restored.State -ne 'Restored' -or (Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash -ne $oldHash) {
    throw 'Standalone rollback did not restore prior plugin'
}
if (!(Test-Path -LiteralPath $sentinel) -or !(Test-Path -LiteralPath $unrelated) -or
    @(Get-ChildItem -LiteralPath $target -Recurse -File).Count -ne 3) { throw 'Rollback changed unrelated application contents' }
# Corrupt only an isolated package copy; previews must reject it without writes.
$badPackage=Join-Path $fixture 'damaged-package'
Copy-Item -LiteralPath $PackageRoot -Destination $badPackage -Recurse
$locale=Join-Path $badPackage 'obs-dlss5\data\locale\en-US.ini'
[IO.File]::AppendAllText($locale,"`ncorrupt fixture`n")
foreach ($layout in @('global','application')) {
    $rejected=$false
    try {
        if ($layout -eq 'global') {
            & (Join-Path $badPackage 'Install.ps1') -PackageRoot $badPackage -Destination (Join-Path $fixture 'not-installed') -BackupRoot (Join-Path $fixture 'not-backed-up') | Out-Null
        } else {
            & (Join-Path $badPackage 'Install-OBS.ps1') -PackageRoot $badPackage -ObsRoot $target -BackupRoot (Join-Path $fixture 'not-backed-up') | Out-Null
        }
    } catch {
        if ($_.Exception.Message -notlike '*Package integrity failure*') { throw }
        $rejected=$true
    }
    if (!$rejected) { throw 'Corrupt payload was accepted' }
}
if (Test-Path -LiteralPath (Join-Path $fixture 'not-backed-up')) { throw 'Integrity failure created a backup' }
[pscustomobject]@{State='Passed';PowerShell=$PSVersionTable.PSVersion.ToString();
    Coverage='verify; application-local preview/install/standalone rollback; unrelated preservation; damaged package rejection';Fixture=$fixture}
