# SPDX-License-Identifier: GPL-3.0-only
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Resolve-NeuralPath([string]$Path) { [IO.Path]::GetFullPath($Path) }
function Join-NeuralOwnedPath([string]$Root,[string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative)) { throw "Expected relative package path: $Relative" }
    $resolvedRoot=(Resolve-NeuralPath $Root).TrimEnd('\','/')
    if ((Test-Path -LiteralPath $resolvedRoot) -and ((Get-Item -LiteralPath $resolvedRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Plugin root cannot be a link: $resolvedRoot"
    }
    $resolved=Resolve-NeuralPath (Join-Path $resolvedRoot $Relative)
    if (!$resolved.StartsWith($resolvedRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes plugin root: $Relative"
    }
    $cursor=$resolved
    while ($cursor -and $cursor.StartsWith($resolvedRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Path crosses a link inside the plugin root: $cursor"
        }
        $cursor=[IO.Path]::GetDirectoryName($cursor)
    }
    $resolved
}
function Get-NeuralHash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Assert-NeuralRuntime([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Runtime file not found: $Path" }
    $bytes=[IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 128 -or [BitConverter]::ToUInt16($bytes,0) -ne 0x5A4D) { throw 'Runtime is not a PE DLL' }
    $pe=[BitConverter]::ToInt32($bytes,0x3C)
    if ($pe -lt 0 -or $pe+264 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550) { throw 'Invalid runtime PE header' }
    if ([BitConverter]::ToUInt16($bytes,$pe+4) -ne 0x8664 -or [BitConverter]::ToUInt16($bytes,$pe+24) -ne 0x20B) { throw 'Runtime must be x64' }
    if (([BitConverter]::ToUInt16($bytes,$pe+22) -band 0x2000) -eq 0) { throw 'Runtime PE image must be a DLL' }
    $count=[BitConverter]::ToUInt16($bytes,$pe+6)
    $sectionStart=$pe+24+[BitConverter]::ToUInt16($bytes,$pe+20)
    if ($sectionStart+$count*40 -gt $bytes.Length) { throw 'Runtime section table is truncated' }
    $sections=@()
    for ($i=0;$i -lt $count;$i++) {
        $o=$sectionStart+$i*40
        $sections+=,[pscustomobject]@{
            VA=[uint64][BitConverter]::ToUInt32($bytes,$o+12)
            Size=[uint64][BitConverter]::ToUInt32($bytes,$o+16)
            Offset=[uint64][BitConverter]::ToUInt32($bytes,$o+20)
        }
    }
    function Convert-NeuralRva([uint32]$Rva,[uint32]$Length=1) {
        foreach ($section in $sections) {
            if ($Rva -ge $section.VA -and [uint64]$Rva+$Length -le $section.VA+$section.Size) {
                $offset=[uint64]$Rva-$section.VA+$section.Offset
                if ($offset+$Length -gt $bytes.Length) { throw 'Runtime export range is truncated' }
                return [int]$offset
            }
        }
        throw 'Runtime export RVA is outside file-backed sections'
    }
    $export=Convert-NeuralRva ([BitConverter]::ToUInt32($bytes,$pe+24+112)) 40
    $namesCount=[BitConverter]::ToUInt32($bytes,$export+24)
    if ($namesCount -gt 65536) { throw 'Runtime export table exceeds bounds' }
    $names=Convert-NeuralRva ([BitConverter]::ToUInt32($bytes,$export+32)) ($namesCount*4)
    $exports=@{}
    for ($i=0;$i -lt $namesCount;$i++) {
        $offset=Convert-NeuralRva ([BitConverter]::ToUInt32($bytes,$names+$i*4))
        $end=$offset
        while ($end -lt $bytes.Length -and $end-$offset -lt 256 -and $bytes[$end] -ne 0) { $end++ }
        if ($end -ge $bytes.Length -or $end-$offset -ge 256) { throw 'Runtime export name is unterminated' }
        $exports[[Text.Encoding]::ASCII.GetString($bytes,$offset,$end-$offset)]=$true
    }
    foreach ($name in @('NVSDK_NGX_D3D12_Init_Ext','NVSDK_NGX_D3D12_CreateFeature',
        'NVSDK_NGX_D3D12_EvaluateFeature','NVSDK_NGX_D3D12_ReleaseFeature','NVSDK_NGX_D3D12_Shutdown1')) {
        if (!$exports.ContainsKey($name)) { throw "Runtime capability is missing: $name" }
    }
}
function Get-NeuralPackageFiles([string]$PluginRoot) {
    $root=(Resolve-NeuralPath $PluginRoot).TrimEnd('\','/')
    foreach ($file in @(Get-ChildItem -LiteralPath $root -File -Recurse)) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package link is not allowed: $($file.FullName)" }
        [pscustomobject]@{ Relative=$file.FullName.Substring($root.Length+1); Source=$file.FullName; SHA256=Get-NeuralHash $file.FullName }
    }
}
function Get-NeuralVerifiedPackageFiles([string]$PackageRoot) {
    $package=Resolve-NeuralPath $PackageRoot
    $root=Join-NeuralOwnedPath $package 'obs-dlss5'
    $manifestPath=Join-NeuralOwnedPath $package 'manifest.json'
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.Plugin -ne 'DLSS5 Neural Filter' -or $manifest.OBSVersionHardGate -ne $false -or
        $manifest.HostVersionListIsAllowlist -ne $false) { throw 'Unknown package or unexpected version restriction' }
    $declared=@{}
    $verified=@()
    foreach ($entry in $manifest.Files) {
        $relative=([string]$entry.Relative).Replace('/','\')
        if (!$relative -or $declared.ContainsKey($relative) -or $entry.SHA256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw 'Invalid or duplicate package checksum entry'
        }
        $file=Join-NeuralOwnedPath $root $relative
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-NeuralHash $file) -ne $entry.SHA256) {
            throw "Package integrity failure: $relative"
        }
        $declared[$relative]=$true
        $verified+=,[pscustomobject]@{Relative=$relative;Source=$file;SHA256=$entry.SHA256}
    }
    if (!$declared.ContainsKey('bin\64bit\obs-dlss5.dll')) { throw 'Manifest is missing the production plugin' }
    foreach ($actual in @(Get-NeuralPackageFiles $root)) {
        if (!$declared.ContainsKey($actual.Relative)) { throw "Unlisted package payload: $($actual.Relative)" }
    }
    $verified
}
