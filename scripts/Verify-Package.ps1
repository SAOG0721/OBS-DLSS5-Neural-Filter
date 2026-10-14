# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([string]$PackageRoot='')
if (!$PackageRoot) { $PackageRoot=$PSScriptRoot }
. (Join-Path $PSScriptRoot 'Delivery.ps1')
$package=Resolve-NeuralPath $PackageRoot
$files=@(Get-NeuralVerifiedPackageFiles $package)
$manifest=Get-Content -LiteralPath (Join-NeuralOwnedPath $package 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.PSObject.Properties.Name -contains 'PackageFiles') {
    foreach ($entry in $manifest.PackageFiles) {
        $file=Join-NeuralOwnedPath $package $entry.Relative
        if ((Get-NeuralHash $file) -ne $entry.SHA256) { throw "Package file integrity failure: $($entry.Relative)" }
    }
}
$runtime=Join-NeuralOwnedPath $package 'obs-dlss5\bin\64bit\runtime\nvngx_dlssnr.dll'
if ($manifest.RuntimeIncluded) { Assert-NeuralRuntime $runtime }
[pscustomobject]@{State='Verified';Plugin=$manifest.Plugin;Version=$manifest.Version;Revision=$manifest.Revision;
    PayloadFiles=$files.Count;RuntimeIncluded=$manifest.RuntimeIncluded;OBSVersionGate=$false}
