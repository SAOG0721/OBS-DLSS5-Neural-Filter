# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Receipt,[switch]$Apply,[switch]$Force)
. (Join-Path $PSScriptRoot 'Delivery.ps1')
$receiptPath=Resolve-NeuralPath $Receipt
$record=Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($record.SchemaVersion -ne 1 -or $record.Plugin -ne 'DLSS5 Neural Filter') { throw 'Unknown rollback receipt' }
$root=Resolve-NeuralPath $record.Destination
$backup=Resolve-NeuralPath $record.BackupDirectory
$actions=@()
foreach ($entry in $record.Entries) {
    $target=Join-NeuralOwnedPath $root $entry.Relative
    $original=Join-NeuralOwnedPath $backup ('original\'+$entry.Relative)
    if ($entry.OriginalSHA256) {
        if (!(Test-Path -LiteralPath $original -PathType Leaf) -or (Get-NeuralHash $original) -ne $entry.OriginalSHA256) { throw "Backup integrity failure: $($entry.Relative)" }
    }
    $modified=(Test-Path -LiteralPath $target -PathType Leaf) -and ((Get-NeuralHash $target) -ne $entry.InstalledSHA256)
    $action=if ($entry.OriginalSHA256) { 'RestoreOriginal' } else { 'RemoveAddedFile' }
    $actions+=,[pscustomobject]@{Relative=$entry.Relative;Target=$target;Original=$original;Action=$action;Modified=$modified}
}
if (!$Apply) {
    [ordered]@{Mode='Preview';Destination=$root;Actions=$actions;Conflicts=@($actions | Where-Object Modified)} | ConvertTo-Json -Depth 5
    return
}
if (!$Force -and @($actions | Where-Object Modified).Count) { throw 'Installed files changed after installation; review preview before using -Force' }
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before restoring plugin files' }
foreach ($action in $actions) {
    if ($action.Action -eq 'RestoreOriginal') {
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($action.Target)) -Force | Out-Null
        Copy-Item -LiteralPath $action.Original -Destination $action.Target -Force
        if ((Get-NeuralHash $action.Target) -ne (Get-NeuralHash $action.Original)) { throw "Restored checksum mismatch: $($action.Relative)" }
    } elseif (Test-Path -LiteralPath $action.Target -PathType Leaf) { Remove-Item -LiteralPath $action.Target -Force }
}
if (Test-Path -LiteralPath $root -PathType Container) {
    # Only consider ancestors of receipt-owned files. An application-root
    # installation must not remove unrelated empty OBS directories.
    $directories=@{}
    $rootPrefix=$root.TrimEnd('\')+'\'
    foreach ($action in $actions) {
        $directory=[IO.Path]::GetDirectoryName($action.Target)
        while ($directory -and $directory.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase)) {
            $directories[$directory]=$true
            $directory=[IO.Path]::GetDirectoryName($directory)
        }
    }
    foreach ($directory in @($directories.Keys | Sort-Object -Descending)) {
        $relative=$directory.Substring($rootPrefix.Length)
        $resolved=Join-NeuralOwnedPath $root $relative
        if ((Test-Path -LiteralPath $resolved -PathType Container) -and @(Get-ChildItem -LiteralPath $resolved -Force).Count -eq 0) {
            Remove-Item -LiteralPath $resolved -Force
        }
    }
}
[pscustomobject]@{State='Restored';Destination=$root;Receipt=$receiptPath}
