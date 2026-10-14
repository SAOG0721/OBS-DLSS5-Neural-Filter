# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture=Join-Path $project ('tests\results\delivery-verified-'+[Guid]::NewGuid().ToString('N'))
$target=Join-Path $fixture 'installed\obs-dlss5'
$backup=Join-Path $fixture 'backups'
$oldPlugin=Join-Path $target 'bin\64bit\obs-dlss5.dll'
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($oldPlugin)) -Force | Out-Null
[IO.File]::WriteAllBytes($oldPlugin,[Text.Encoding]::UTF8.GetBytes('existing plugin fixture'))
$originalHash=(Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash
$sentinel=Join-Path $target 'user-note.txt'
[IO.File]::WriteAllText($sentinel,'preserve destination-only files')
$preview=& (Join-Path $PackageRoot 'Install.ps1') -PackageRoot $PackageRoot -Destination $target -BackupRoot $backup
if ((Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash -ne $originalHash -or (Test-Path -LiteralPath $backup)) {
    throw 'Preview wrote to installed files or created a backup'
}
$result=& (Join-Path $PackageRoot 'Install.ps1') -PackageRoot $PackageRoot -Destination $target -BackupRoot $backup -Apply
if ($result.State -ne 'Installed') { throw 'Installation did not finish' }
$receipt=Get-Content -LiteralPath $result.Receipt -Raw -Encoding UTF8 | ConvertFrom-Json
if ($receipt.State -ne 'Installed' -or !$receipt.Entries.Count) { throw 'Backup receipt is incomplete' }
foreach ($entry in $receipt.Entries) {
    $file=Join-Path $target $entry.Relative
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.InstalledSHA256) { throw 'Installed checksum differs from receipt' }
}
$restored=& (Join-Path $PackageRoot 'Restore.ps1') -Receipt $result.Receipt -Apply
if ($restored.State -ne 'Restored' -or (Get-FileHash -LiteralPath $oldPlugin -Algorithm SHA256).Hash -ne $originalHash) {
    throw 'Rollback did not restore the prior plugin'
}
if (!(Test-Path -LiteralPath $sentinel) -or (Get-ChildItem -LiteralPath $target -Recurse -File).Count -ne 2) {
    throw 'Rollback removed destination-only files or left added package files'
}
[pscustomobject]@{State='Passed'; PowerShell=$PSVersionTable.PSVersion.ToString(); Fixture=$fixture; Receipt=$result.Receipt}
