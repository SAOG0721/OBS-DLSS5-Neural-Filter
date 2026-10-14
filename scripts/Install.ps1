# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [string]$PackageRoot='',
    [string]$Destination=(Join-Path $env:ProgramData 'obs-studio\plugins\obs-dlss5'),
    [string]$RuntimeDll='',
    [string]$BackupRoot=(Join-Path $env:LOCALAPPDATA 'Dlss5NeuralFilter\Backups'),
    [switch]$Apply
)
if (!$PackageRoot) { $PackageRoot=$PSScriptRoot }
. (Join-Path $PSScriptRoot 'Delivery.ps1')
$package=Resolve-NeuralPath $PackageRoot
$source=Join-Path $package 'obs-dlss5'
$target=Resolve-NeuralPath $Destination
$backupBase=Resolve-NeuralPath $BackupRoot
$pluginFile=Join-NeuralOwnedPath $source 'bin\64bit\obs-dlss5.dll'
if (!(Test-Path -LiteralPath $pluginFile -PathType Leaf)) { throw 'Package is missing obs-dlss5/bin/64bit/obs-dlss5.dll' }
if ($target -eq (Resolve-NeuralPath $source) -or $target.StartsWith((Resolve-NeuralPath $source)+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Install destination must be outside the package source'
}
if ($backupBase -eq $target -or $backupBase.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Backups must be outside the installed plugin directory' }
$files=@(Get-NeuralVerifiedPackageFiles $package)
if ($RuntimeDll) {
    $selected=Resolve-NeuralPath $RuntimeDll
    Assert-NeuralRuntime $selected
    $relative='bin\64bit\runtime\nvngx_dlssnr.dll'
    $files=@($files | Where-Object Relative -ne $relative)
    $files+=,[pscustomobject]@{ Relative=$relative; Source=$selected; SHA256=Get-NeuralHash $selected }
} else {
    $selected=Join-NeuralOwnedPath $source 'bin\64bit\runtime\nvngx_dlssnr.dll'
    if (Test-Path -LiteralPath $selected -PathType Leaf) { Assert-NeuralRuntime $selected }
}
foreach ($file in $files) { $null=Join-NeuralOwnedPath $target $file.Relative }
$preview=[ordered]@{ Mode='Preview'; Plugin='DLSS5 Neural Filter'; Destination=$target; OBSVersionGate=$false;
    Files=@($files | Select-Object Relative,SHA256); SelectedRuntime=$selected; BackupRoot=$backupBase }
if (!$Apply) { $preview | ConvertTo-Json -Depth 5; return }
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before installing plugin files' }
$id=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$backup=Join-Path $backupBase $id
New-Item -ItemType Directory -Path $backup -Force | Out-Null
foreach ($script in @('Restore.ps1','Delivery.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $backup
}
$entries=@()
$receiptPath=Join-Path $backup 'receipt.json'
function Write-NeuralReceipt([string]$currentState) {
    [ordered]@{ SchemaVersion=1; State=$currentState; Plugin='DLSS5 Neural Filter'; Destination=$target;
        BackupDirectory=$backup; Entries=$entries; CreatedAt=(Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
}
Write-NeuralReceipt 'Installing'
try {
    foreach ($file in $files) {
        $destinationFile=Join-NeuralOwnedPath $target $file.Relative
        $originalHash=$null
        if (Test-Path -LiteralPath $destinationFile -PathType Leaf) {
            $originalHash=Get-NeuralHash $destinationFile
            $backupFile=Join-NeuralOwnedPath $backup ('original\'+$file.Relative)
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backupFile)) -Force | Out-Null
            Copy-Item -LiteralPath $destinationFile -Destination $backupFile
            if ((Get-NeuralHash $backupFile) -ne $originalHash) { throw 'Backup checksum mismatch' }
        }
        $entries+=,[pscustomobject]@{ Relative=$file.Relative; OriginalSHA256=$originalHash; InstalledSHA256=$file.SHA256 }
        Write-NeuralReceipt 'Installing'
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destinationFile)) -Force | Out-Null
        Copy-Item -LiteralPath $file.Source -Destination $destinationFile -Force
        if ((Get-NeuralHash $destinationFile) -ne $file.SHA256) { throw "Installed checksum mismatch: $($file.Relative)" }
    }
    $state='Installed'
} catch { $state='Interrupted'; $failure=$_ }
Write-NeuralReceipt $state
[pscustomobject]@{State=$state;Destination=$target;Receipt=$receiptPath;SelectedRuntime=$selected}
if ($state -eq 'Interrupted') { throw $failure }
