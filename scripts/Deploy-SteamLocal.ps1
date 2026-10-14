# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ObsRoot,
    [string]$PackageRoot='',
    [string]$BackupRoot=(Join-Path $env:LOCALAPPDATA 'Dlss5NeuralFilter\Backups'),
    [switch]$Apply
)
if (!$PackageRoot) { $PackageRoot=Join-Path $PSScriptRoot '..\dist\0.1.0-dev' }
. (Join-Path $PSScriptRoot 'Delivery.ps1')
$target=Resolve-NeuralPath $ObsRoot
$package=Resolve-NeuralPath $PackageRoot
$source=Join-Path $package 'obs-dlss5'
$exe=Join-NeuralOwnedPath $target 'bin\64bit\obs64.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Target is not an OBS application installation' }
$backupBase=Resolve-NeuralPath $BackupRoot
if ($target -eq (Resolve-NeuralPath $source) -or $target.StartsWith((Resolve-NeuralPath $source)+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Install destination must be outside the package source'
}
if ($backupBase -eq $target -or $backupBase.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Backups must be outside the OBS application directory'
}
$files=@()
foreach ($entry in @(Get-NeuralVerifiedPackageFiles $package)) {
    $original=$entry.Source
    if ($entry.Relative -eq 'bin\64bit\obs-dlss5.dll') { $relative='obs-plugins\64bit\obs-dlss5.dll' }
    elseif ($entry.Relative.StartsWith('bin\64bit\runtime\',[StringComparison]::OrdinalIgnoreCase)) {
        $relative='obs-plugins\64bit\runtime\'+$entry.Relative.Substring('bin\64bit\runtime\'.Length)
    } elseif ($entry.Relative.StartsWith('data\',[StringComparison]::OrdinalIgnoreCase)) {
        $relative='data\obs-plugins\obs-dlss5\'+$entry.Relative.Substring('data\'.Length)
    } else { throw "Unexpected package file: $($entry.Relative)" }
    $null=Join-NeuralOwnedPath $target $relative
    $files+=,[pscustomobject]@{Relative=$relative;Source=$original;SHA256=$entry.SHA256}
}
Assert-NeuralRuntime (Join-Path $source 'bin\64bit\runtime\nvngx_dlssnr.dll')
$plan=[ordered]@{Mode='Preview';Plugin='DLSS5 Neural Filter';Layout='Steam application-local';
    Destination=$target;OBSVersionGate=$false;Files=@($files | Select-Object Relative,SHA256)}
if (!$Apply) { $plan | ConvertTo-Json -Depth 5; return }
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before deploying plugin files' }
$id='Steam-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$backup=Join-Path $backupBase $id
New-Item -ItemType Directory -Path $backup -Force | Out-Null
foreach ($file in @('Restore.ps1','Delivery.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $backup
}
$receiptPath=Join-Path $backup 'receipt.json'
$entries=@()
function Save-SteamReceipt([string]$state) {
    [ordered]@{SchemaVersion=1;State=$state;Plugin='DLSS5 Neural Filter';Destination=$target;
        BackupDirectory=$backup;Entries=$entries;Layout='Steam application-local';
        CreatedAt=(Get-Date).ToUniversalTime().ToString('o')} | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $receiptPath -Encoding UTF8
}
Save-SteamReceipt 'Installing'
try {
    foreach ($file in $files) {
        $destination=Join-NeuralOwnedPath $target $file.Relative
        $originalHash=$null
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            $originalHash=Get-NeuralHash $destination
            $saved=Join-NeuralOwnedPath $backup ('original\'+$file.Relative)
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($saved)) -Force | Out-Null
            Copy-Item -LiteralPath $destination -Destination $saved
            if ((Get-NeuralHash $saved) -ne $originalHash) { throw 'Backup integrity failure' }
        }
        $entries+=,[pscustomobject]@{Relative=$file.Relative;OriginalSHA256=$originalHash;InstalledSHA256=$file.SHA256}
        Save-SteamReceipt 'Installing'
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
        Copy-Item -LiteralPath $file.Source -Destination $destination -Force
        if ((Get-NeuralHash $destination) -ne $file.SHA256) { throw 'Deployed file checksum mismatch' }
    }
    Save-SteamReceipt 'Installed'
} catch { Save-SteamReceipt 'Interrupted'; throw }
[pscustomobject]@{State='Installed';Destination=$target;Receipt=$receiptPath;Files=$entries.Count}
