# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ObsRoot,
    [string]$PackageRoot='',
    [string]$BackupRoot=(Join-Path $env:LOCALAPPDATA 'Dlss5NeuralFilter\Backups'),
    [switch]$Apply
)
if (!$PackageRoot) { $PackageRoot=$PSScriptRoot }
# Application-local layout works with Steam and portable OBS installations.
& (Join-Path $PSScriptRoot 'Deploy-SteamLocal.ps1') -ObsRoot $ObsRoot -PackageRoot $PackageRoot -BackupRoot $BackupRoot -Apply:$Apply
