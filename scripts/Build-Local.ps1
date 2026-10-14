# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ObsSourceRoot,
    [Parameter(Mandatory)][string]$NgxSdkRoot,
    [Parameter(Mandatory)][string]$FidelityFxSdkRoot,
    [Parameter(Mandatory)][string]$NvOfSdkRoot,
    [Parameter(Mandatory)][string]$DxcPath,
    [string]$RuntimeDll='',
    [string]$PackageVersion='0.1.0',
    [switch]$Package
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cmakeCommand=Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmake=$cmakeCommand.Source } else {
    $locator=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs=(& $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (!$vs) { throw 'Visual Studio C++ tools are missing' }
    $cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$build=Join-Path $project 'build'
$configure=@('-S',$project,'-B',$build,'-G','Visual Studio 17 2022','-A','x64',
    "-DOBS_SOURCE_ROOT=$ObsSourceRoot","-DNGX_SDK_ROOT=$NgxSdkRoot","-DFIDELITYFX_SDK_ROOT=$FidelityFxSdkRoot",
    "-DNVOF_SDK_ROOT=$NvOfSdkRoot","-DDXC_PATH=$DxcPath")
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& $cmake --build $build --config Release --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
$results=Join-Path $project 'tests\results'
New-Item -ItemType Directory -Path $results -Force | Out-Null
$ctest=Join-Path ([IO.Path]::GetDirectoryName($cmake)) 'ctest.exe'
& $ctest --test-dir $build -C Release --output-on-failure --output-junit (Join-Path $results 'ctest-release.xml')
if ($LASTEXITCODE -ne 0) { throw 'Contract tests failed' }
if ($RuntimeDll) {
    & (Join-Path $build 'Release\gpu_contracts.exe') --neural 1280 720 $RuntimeDll |
        Tee-Object -FilePath (Join-Path $results 'neural-runtime.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Selected runtime failed native GPU tests' }
}
if (!$Package) { return }
$packageArgs=@('--version',$PackageVersion)
if ($RuntimeDll) { $packageArgs+=@('--runtime',$RuntimeDll) }
& python (Join-Path $PSScriptRoot 'Prepare-Distribution.py') @packageArgs
if ($LASTEXITCODE -ne 0) { throw 'Distribution preparation failed' }
