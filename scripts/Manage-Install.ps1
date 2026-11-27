# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall')][string]$Mode = 'Install',
    [string]$ObsRoot = '',
    [string]$PackageRoot = '',
    [string]$BackupRoot = (Join-Path $env:LOCALAPPDATA 'Dlss5NeuralFilter\Backups'),
    [string]$LegacyProgramDataDir = (Join-Path $env:ProgramData 'obs-studio\plugins\obs-dlss5'),
    [switch]$Apply,
    [switch]$NonInteractive,
    [switch]$CleanLegacyProgramData,
    [string]$TranscriptLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$IsZh = [Globalization.CultureInfo]::CurrentUICulture.Name -like 'zh*'
function Msg([string]$Zh, [string]$En) {
    if ($IsZh) { return $Zh }
    return $En
}

function Write-Ui([string]$Text = '', [ConsoleColor]$Color = [ConsoleColor]::Gray) {
    if (-not $NonInteractive -or $TranscriptLog) {
        if ($Color -eq [ConsoleColor]::Gray) {
            Write-Host $Text
        } else {
            Write-Host $Text -ForegroundColor $Color
        }
        if ($TranscriptLog) {
            [IO.File]::AppendAllText($TranscriptLog, $Text + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        }
    }
}

function Resolve-FullPath([string]$Path) {
    [IO.Path]::GetFullPath($Path.Trim().Trim('"'))
}

function Join-SafeChildPath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative)) { throw "Expected relative path: $Relative" }
    $resolvedRoot = (Resolve-FullPath $Root).TrimEnd('\', '/')
    if ((Test-Path -LiteralPath $resolvedRoot) -and
        ((Get-Item -LiteralPath $resolvedRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Root path cannot be a reparse point/symlink: $resolvedRoot"
    }
    $resolved = Resolve-FullPath (Join-Path $resolvedRoot $Relative)
    if (-not $resolved.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes root directory: $Relative"
    }
    $cursor = $resolved
    while ($cursor -and $cursor.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Path crosses a reparse point/symlink: $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    $resolved
}

function Get-Sha256Upper([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Assert-NgxRuntimeDll([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw (Msg "未找到运行库文件: $Path" "Runtime DLL not found: $Path")
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 128 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) {
        throw "Runtime is not a PE DLL: $Path"
    }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 264 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) {
        throw 'Invalid runtime PE header'
    }
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20B) {
        throw 'Runtime must be x64'
    }
    if (([BitConverter]::ToUInt16($bytes, $pe + 22) -band 0x2000) -eq 0) {
        throw 'Runtime PE image must be a DLL'
    }
    $count = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $sectionStart = $pe + 24 + [BitConverter]::ToUInt16($bytes, $pe + 20)
    if ($sectionStart + $count * 40 -gt $bytes.Length) {
        throw 'Runtime section table is truncated'
    }
    $sections = @()
    for ($i = 0; $i -lt $count; $i++) {
        $o = $sectionStart + $i * 40
        $sections += ,[pscustomobject]@{
            VA     = [uint64][BitConverter]::ToUInt32($bytes, $o + 12)
            Size   = [uint64][BitConverter]::ToUInt32($bytes, $o + 16)
            Offset = [uint64][BitConverter]::ToUInt32($bytes, $o + 20)
        }
    }
    function Convert-Rva([uint32]$Rva, [uint32]$Length = 1) {
        foreach ($section in $sections) {
            if ($Rva -ge $section.VA -and [uint64]$Rva + $Length -le $section.VA + $section.Size) {
                $offset = [uint64]$Rva - $section.VA + $section.Offset
                if ($offset + $Length -gt $bytes.Length) { throw 'Runtime export range is truncated' }
                return [int]$offset
            }
        }
        throw 'Runtime export RVA is outside file-backed sections'
    }
    $export = Convert-Rva ([BitConverter]::ToUInt32($bytes, $pe + 24 + 112)) 40
    $namesCount = [BitConverter]::ToUInt32($bytes, $export + 24)
    if ($namesCount -gt 65536) { throw 'Runtime export table exceeds bounds' }
    $names = Convert-Rva ([BitConverter]::ToUInt32($bytes, $export + 32)) ($namesCount * 4)
    $exports = @{}
    for ($i = 0; $i -lt $namesCount; $i++) {
        $offset = Convert-Rva ([BitConverter]::ToUInt32($bytes, $names + $i * 4))
        $end = $offset
        while ($end -lt $bytes.Length -and $end - $offset -lt 256 -and $bytes[$end] -ne 0) { $end++ }
        if ($end -ge $bytes.Length -or $end - $offset -ge 256) { throw 'Runtime export name is unterminated' }
        $exports[[Text.Encoding]::ASCII.GetString($bytes, $offset, $end - $offset)] = $true
    }
    foreach ($name in @(
        'NVSDK_NGX_D3D12_Init_Ext',
        'NVSDK_NGX_D3D12_CreateFeature',
        'NVSDK_NGX_D3D12_EvaluateFeature',
        'NVSDK_NGX_D3D12_ReleaseFeature',
        'NVSDK_NGX_D3D12_Shutdown1')) {
        if (-not $exports.ContainsKey($name)) { throw "Runtime capability is missing: $name" }
    }
}

function Resolve-ObsRootInput([string]$InputPath) {
    if (-not $InputPath -or -not $InputPath.Trim()) {
        throw (Msg 'OBS 安装路径不能为空。' 'OBS root path cannot be empty.')
    }
    $candidate = Resolve-FullPath $InputPath
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $candidate = [IO.Path]::GetDirectoryName($candidate)
    }
    $cursor = $candidate
    for ($depth = 0; $depth -lt 4 -and $cursor; $depth++) {
        $exe = Join-Path $cursor 'bin\64bit\obs64.exe'
        if (Test-Path -LiteralPath $exe -PathType Leaf) {
            return $cursor
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    throw (Msg "该目录下未找到 bin\64bit\obs64.exe，不是有效的 OBS 安装根目录: $candidate" "Could not find bin\64bit\obs64.exe under: $candidate")
}

function Find-ObsInstallations {
    $candidates = [System.Collections.Generic.List[string]]::new()

    foreach ($regPath in @(
        'HKLM:\SOFTWARE\OBS Studio',
        'HKLM:\SOFTWARE\WOW6432Node\OBS Studio'
    )) {
        try {
            if (Test-Path -LiteralPath $regPath) {
                $val = (Get-ItemProperty -LiteralPath $regPath -ErrorAction SilentlyContinue).'(default)'
                if ($val) { $candidates.Add([string]$val) }
            }
        } catch {}
    }

    foreach ($uninstPath in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio'
    )) {
        try {
            if (Test-Path -LiteralPath $uninstPath) {
                $props = Get-ItemProperty -LiteralPath $uninstPath -ErrorAction SilentlyContinue
                if ($props.PSObject.Properties.Name -contains 'InstallLocation' -and $props.InstallLocation) {
                    $candidates.Add([string]$props.InstallLocation)
                }
                if ($props.PSObject.Properties.Name -contains 'UninstallString' -and $props.UninstallString) {
                    $u = ([string]$props.UninstallString).Trim().Trim('"')
                    if ($u) {
                        $uDir = [IO.Path]::GetDirectoryName($u)
                        if ($uDir) { $candidates.Add($uDir) }
                    }
                }
            }
        } catch {}
    }

    $candidates.Add((Join-Path $env:ProgramFiles 'obs-studio'))
    if (${env:ProgramFiles(x86)}) {
        $candidates.Add((Join-Path ${env:ProgramFiles(x86)} 'obs-studio'))
    }

    $steamRoots = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($steamReg in @(
        @{ Path = 'HKCU:\SOFTWARE\Valve\Steam'; Name = 'SteamPath' },
        @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Name = 'InstallPath' },
        @{ Path = 'HKLM:\SOFTWARE\Valve\Steam'; Name = 'InstallPath' }
    )) {
        try {
            if (Test-Path -LiteralPath $steamReg.Path) {
                $p = Get-ItemProperty -LiteralPath $steamReg.Path -Name $steamReg.Name -ErrorAction SilentlyContinue
                $val = $p.($steamReg.Name)
                if ($val) { $null = $steamRoots.Add((Resolve-FullPath ([string]$val).Replace('/', '\'))) }
            }
        } catch {}
    }

    $libraryFolders = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($sr in $steamRoots) {
        $null = $libraryFolders.Add($sr)
        $vdf = Join-Path $sr 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf -PathType Leaf) {
            try {
                $text = [IO.File]::ReadAllText($vdf, [Text.Encoding]::UTF8)
                foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                    $lib = $m.Groups[1].Value.Replace('\\', '\')
                    if ($lib) { $null = $libraryFolders.Add((Resolve-FullPath $lib)) }
                }
            } catch {}
        }
    }

    foreach ($lib in $libraryFolders) {
        $candidates.Add((Join-Path $lib 'steamapps\common\OBS Studio'))
    }

    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $results = @()
    foreach ($raw in $candidates) {
        if (-not $raw -or -not $raw.Trim()) { continue }
        try {
            $full = Resolve-FullPath $raw
        } catch { continue }
        if (-not $seen.Add($full)) { continue }
        $exe = Join-Path $full 'bin\64bit\obs64.exe'
        if (Test-Path -LiteralPath $exe -PathType Leaf) {
            $kind = if ($full -match '(?i)\\steamapps\\common\\') { 'Steam' } else { 'Standard' }
            $hasPlugin = Test-Path -LiteralPath (Join-Path $full 'obs-plugins\64bit\obs-dlss5.dll') -PathType Leaf
            $results += ,[pscustomobject]@{
                Root      = $full
                Kind      = $kind
                HasPlugin = $hasPlugin
            }
        }
    }
    return $results
}

function Select-FolderDialog([string]$Description) {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = $Description
        $dialog.ShowNewFolderButton = $false
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.SelectedPath
        }
        return ''
    } catch {
        $typed = Read-Host (Msg '请输入 OBS 安装根目录路径（包含 bin\64bit\obs64.exe）' 'Enter the OBS root directory (containing bin\64bit\obs64.exe)')
        return $typed
    }
}

function Test-ObsRunningForTarget([string]$TargetRoot, [bool]$TouchesSystemProgramData = $false) {
    $procs = @(Get-Process -Name obs64 -ErrorAction SilentlyContinue)
    if (-not $procs.Count) { return $false }
    if ($TouchesSystemProgramData -or -not $TargetRoot) { return $true }
    $prefix = (Resolve-FullPath $TargetRoot).TrimEnd('\', '/') + '\'
    foreach ($p in $procs) {
        $exePath = $null
        try { $exePath = $p.Path } catch {}
        if (-not $exePath) { return $true }
        if ((Resolve-FullPath $exePath).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-DirectoryWritable([string]$DirPath) {
    $probeDir = $DirPath
    while ($probeDir -and -not (Test-Path -LiteralPath $probeDir -PathType Container)) {
        $probeDir = [IO.Path]::GetDirectoryName($probeDir)
    }
    if (-not $probeDir) { return $false }
    $probeFile = Join-Path $probeDir ('.dlss5-access-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($probeFile, '')
        Remove-Item -LiteralPath $probeFile -Force -ErrorAction SilentlyContinue
        return $true
    } catch {
        return $false
    }
}

function Get-InstallPayloadFiles([string]$ResolvedPackageRoot) {
    $obsPluginsRoot = Join-SafeChildPath $ResolvedPackageRoot 'obs-plugins'
    $dataRoot = Join-SafeChildPath $ResolvedPackageRoot 'data'
    $pluginBinRoot = Join-SafeChildPath $ResolvedPackageRoot 'obs-plugins\64bit'
    $pluginDataRoot = Join-SafeChildPath $ResolvedPackageRoot 'data\obs-plugins\obs-dlss5'

    $mainDll = Join-SafeChildPath $ResolvedPackageRoot 'obs-plugins\64bit\obs-dlss5.dll'
    if (-not (Test-Path -LiteralPath $mainDll -PathType Leaf)) {
        throw (Msg "安装包缺少核心插件: obs-plugins\64bit\obs-dlss5.dll" "Package is missing obs-plugins\64bit\obs-dlss5.dll")
    }
    $runtimeDll = Join-SafeChildPath $ResolvedPackageRoot 'obs-plugins\64bit\runtime\nvngx_dlssnr.dll'
    Assert-NgxRuntimeDll $runtimeDll

    $files = @()
    foreach ($searchRoot in @($obsPluginsRoot, $dataRoot)) {
        if (-not (Test-Path -LiteralPath $searchRoot -PathType Container)) {
            throw (Msg "安装包缺少目录: $searchRoot" "Package is missing directory: $searchRoot")
        }
        foreach ($item in @(Get-ChildItem -LiteralPath $searchRoot -File -Recurse)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Package link is not allowed: $($item.FullName)"
            }
            $full = Resolve-FullPath $item.FullName
            $allowedPrefix = ($full.StartsWith($pluginBinRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
                              $full.StartsWith($pluginDataRoot + '\', [StringComparison]::OrdinalIgnoreCase))
            if (-not $allowedPrefix) {
                throw "Unexpected file outside owned plugin payload: $($item.FullName)"
            }
            if ($item.Extension -match '^(?i)\.(exe|pdb|lib|exp|cmd|ps1)$') {
                throw "Disallowed payload artifact in package: $($item.Name)"
            }
            $relative = $full.Substring($ResolvedPackageRoot.TrimEnd('\', '/').Length + 1)
            $files += ,[pscustomobject]@{
                Relative = $relative
                Source   = $full
                SHA256   = Get-Sha256Upper $full
            }
        }
    }
    if (-not $files.Count) {
        throw 'No payload files found in package'
    }
    return ($files | Sort-Object Relative)
}

function Invoke-ElevatedChild(
    [string]$ResolvedTarget,
    [string]$ResolvedPackageRoot,
    [string]$ResolvedBackupBase,
    [bool]$DoCleanLegacy
) {
    $logFile = Join-Path $env:TEMP ('dlss5-install-' + [Guid]::NewGuid().ToString('N') + '.log')
    $argTokens = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"' + $PSCommandPath + '"'),
        '-Mode', $Mode,
        '-ObsRoot', ('"' + $ResolvedTarget + '"'),
        '-PackageRoot', ('"' + $ResolvedPackageRoot + '"'),
        '-BackupRoot', ('"' + $ResolvedBackupBase + '"'),
        '-LegacyProgramDataDir', ('"' + $LegacyProgramDataDir + '"'),
        '-Apply',
        '-NonInteractive',
        '-TranscriptLog', ('"' + $logFile + '"')
    )
    if ($DoCleanLegacy) { $argTokens += '-CleanLegacyProgramData' }

    Write-Ui (Msg '目标目录需要管理员权限，请在弹出的 UAC 窗口中确认继续...' 'Administrator permission is required for this OBS folder. Please approve the UAC prompt...') Cyan
    $proc = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList ($argTokens -join ' ') -Verb RunAs -Wait -PassThru
    if (Test-Path -LiteralPath $logFile -PathType Leaf) {
        $lines = Get-Content -LiteralPath $logFile -Encoding UTF8 -ErrorAction SilentlyContinue
        foreach ($line in $lines) { Write-Host $line }
        Remove-Item -LiteralPath $logFile -Force -ErrorAction SilentlyContinue
    }
    if ($proc.ExitCode -ne 0) {
        throw (Msg "管理员权限操作失败（退出码 $($proc.ExitCode)）。" "Elevated operation failed with exit code $($proc.ExitCode).")
    }
}

try {
    if (-not $PackageRoot) {
        $folderName = [IO.Path]::GetFileName($PSScriptRoot.TrimEnd('\', '/'))
        if ($folderName -ieq 'installer') {
            $PackageRoot = [IO.Path]::GetDirectoryName($PSScriptRoot.TrimEnd('\', '/'))
        } else {
            $PackageRoot = $PSScriptRoot
        }
    }
    $resolvedPackage = Resolve-FullPath $PackageRoot
    $resolvedBackupBase = Resolve-FullPath $BackupRoot
    $resolvedLegacyDir = if ($LegacyProgramDataDir) { Resolve-FullPath $LegacyProgramDataDir } else { '' }
    $legacyExists = ($resolvedLegacyDir -and (Test-Path -LiteralPath $resolvedLegacyDir -PathType Container))

    if (-not $NonInteractive) {
        $banner = if ($Mode -eq 'Install') {
            Msg '=== DLSS5 Neural Filter 安装向导 ===' '=== DLSS5 Neural Filter Installer ==='
        } else {
            Msg '=== DLSS5 Neural Filter 卸载向导 ===' '=== DLSS5 Neural Filter Uninstaller ==='
        }
        Write-Ui $banner Cyan
    }

    $target = ''
    $legacyOnlyUninstall = $false

    if ($ObsRoot) {
        $target = Resolve-ObsRootInput $ObsRoot
    } elseif ($NonInteractive) {
        throw 'ObsRoot is required in -NonInteractive mode'
    } else {
        $detected = @(Find-ObsInstallations)
        if ($Mode -eq 'Uninstall') {
            $installedObs = @($detected | Where-Object { $_.HasPlugin })
            if ($installedObs.Count -gt 0) {
                $detected = $installedObs
            } elseif ($legacyExists -and $detected.Count -eq 0) {
                $legacyOnlyUninstall = $true
            }
        }

        if (-not $legacyOnlyUninstall) {
            if ($detected.Count -eq 1) {
                $item = $detected[0]
                Write-Ui ''
                Write-Ui (Msg "已自动检测到 OBS 安装目录 [$($item.Kind)]:" "Detected OBS installation [$($item.Kind)]:") Green
                Write-Ui "  $($item.Root)" White
                $choice = Read-Host (Msg '按 Enter 确认使用此目录，输入 B 手动浏览其他文件夹，或输入 Q 取消' 'Press Enter to confirm, B to browse another folder, or Q to cancel')
                if ($choice -match '^[Qq]$') {
                    Write-Ui (Msg '已取消。' 'Cancelled.') Yellow
                    exit 0
                } elseif ($choice -match '^[Bb]$') {
                    $picked = Select-FolderDialog (Msg '请选择 OBS Studio 安装根目录（包含 bin\64bit\obs64.exe）' 'Select your OBS Studio root folder (containing bin\64bit\obs64.exe)')
                    if (-not $picked) { exit 0 }
                    $target = Resolve-ObsRootInput $picked
                } else {
                    $target = $item.Root
                }
            } elseif ($detected.Count -gt 1) {
                Write-Ui ''
                Write-Ui (Msg '检测到多个 OBS 安装目录，请选择目标：' 'Multiple OBS installations detected. Please choose one:') Green
                for ($i = 0; $i -lt $detected.Count; $i++) {
                    $d = $detected[$i]
                    $tag = if ($d.HasPlugin) { Msg ' [已安装本插件]' ' [plugin installed]' } else { '' }
                    Write-Ui ("  [{0}] [{1}] {2}{3}" -f ($i + 1), $d.Kind, $d.Root, $tag)
                }
                Write-Ui (Msg '  [B] 手动浏览其他目录（如便携版）' '  [B] Browse for another folder (e.g. portable OBS)')
                Write-Ui (Msg '  [Q] 取消' '  [Q] Cancel')
                $choice = Read-Host (Msg '请输入编号或字母' 'Enter choice')
                if ($choice -match '^[Qq]$') {
                    Write-Ui (Msg '已取消。' 'Cancelled.') Yellow
                    exit 0
                } elseif ($choice -match '^[Bb]$') {
                    $picked = Select-FolderDialog (Msg '请选择 OBS Studio 安装根目录（包含 bin\64bit\obs64.exe）' 'Select your OBS Studio root folder (containing bin\64bit\obs64.exe)')
                    if (-not $picked) { exit 0 }
                    $target = Resolve-ObsRootInput $picked
                } else {
                    $idx = 0
                    if (-not [int]::TryParse($choice, [ref]$idx) -or $idx -lt 1 -or $idx -gt $detected.Count) {
                        throw (Msg '无效的选择。' 'Invalid selection.')
                    }
                    $target = $detected[$idx - 1].Root
                }
            } else {
                Write-Ui ''
                Write-Ui (Msg '未自动检测到 OBS 安装目录（如果您使用的是便携版 OBS，请在弹窗中选择 OBS 根目录，或直接把 OBS 文件夹拖到 Install.cmd 上）。' 'No OBS installation was auto-detected. Select your OBS root folder in the dialog (or drag your OBS folder onto Install.cmd).') Yellow
                $picked = Select-FolderDialog (Msg '请选择 OBS Studio 安装根目录（包含 bin\64bit\obs64.exe）' 'Select your OBS Studio root folder (containing bin\64bit\obs64.exe)')
                if (-not $picked) { exit 0 }
                $target = Resolve-ObsRootInput $picked
            }
        }
    }

    if ($target) {
        if ($target -eq $resolvedPackage -or $target.StartsWith($resolvedPackage + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Target OBS root cannot be inside the extracted package directory'
        }
        if ($resolvedBackupBase -eq $target -or $resolvedBackupBase.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Backup directory cannot be inside the OBS directory'
        }
    }

    $doLegacyAction = $false
    if ($legacyExists) {
        if ($NonInteractive) {
            $doLegacyAction = [bool]$CleanLegacyProgramData
        } elseif ($Mode -eq 'Install') {
            Write-Ui ''
            Write-Ui (Msg "检测到旧版安装残留: $resolvedLegacyDir" "Detected legacy installation: $resolvedLegacyDir") Yellow
            $ans = Read-Host (Msg '是否自动备份并移出旧版目录，避免 OBS 重复加载两个插件？[Y/n] (默认 Y)' 'Back up and remove the legacy folder to avoid duplicate plugin loading? [Y/n] (default Y)')
            $doLegacyAction = ($ans -notmatch '^[Nn]$')
        } else {
            Write-Ui ''
            Write-Ui (Msg "同时检测到旧版安装目录: $resolvedLegacyDir" "Legacy installation also found: $resolvedLegacyDir") Yellow
            $ans = Read-Host (Msg '是否一并移除旧版安装目录？[Y/n] (默认 Y)' 'Remove the legacy installation directory as well? [Y/n] (default Y)')
            $doLegacyAction = ($ans -notmatch '^[Nn]$')
        }
    }

    if ($Apply) {
        $systemProgramDataPrefix = (Resolve-FullPath $env:ProgramData).TrimEnd('\', '/') + '\'
        $touchesSystemProgramData = [bool]($doLegacyAction -and $resolvedLegacyDir -and $resolvedLegacyDir.StartsWith($systemProgramDataPrefix, [StringComparison]::OrdinalIgnoreCase))
        if ($NonInteractive) {
            if (Test-ObsRunningForTarget $target $touchesSystemProgramData) {
                throw 'Close OBS before modifying plugin files'
            }
        } else {
            while (Test-ObsRunningForTarget $target $touchesSystemProgramData) {
                Write-Ui ''
                Write-Ui (Msg '检测到目标 OBS (obs64.exe) 正在运行，请先完全关闭 OBS 后按 Enter 继续（输入 Q 取消）。' 'Target OBS (obs64.exe) is currently running. Close OBS completely and press Enter to continue (or Q to cancel).') Yellow
                $ans = Read-Host
                if ($ans -match '^[Qq]$') {
                    Write-Ui (Msg '已取消。' 'Cancelled.') Yellow
                    exit 0
                }
            }
        }

        if (-not $NonInteractive -and -not (Test-IsAdministrator)) {
            $needElevation = $false
            if ($target -and -not (Test-DirectoryWritable (Join-Path $target 'obs-plugins\64bit'))) {
                $needElevation = $true
            }
            if ($doLegacyAction -and $resolvedLegacyDir -and -not (Test-DirectoryWritable ([IO.Path]::GetDirectoryName($resolvedLegacyDir)))) {
                $needElevation = $true
            }
            if ($needElevation) {
                if ($legacyOnlyUninstall) {
                    $dummyTarget = $resolvedLegacyDir
                    Invoke-ElevatedChild -ResolvedTarget $dummyTarget -ResolvedPackageRoot $resolvedPackage -ResolvedBackupBase $resolvedBackupBase -DoCleanLegacy $true
                } else {
                    Invoke-ElevatedChild -ResolvedTarget $target -ResolvedPackageRoot $resolvedPackage -ResolvedBackupBase $resolvedBackupBase -DoCleanLegacy $doLegacyAction
                }
                exit 0
            }
        }
    }

    if ($Mode -eq 'Install') {
        $payload = @(Get-InstallPayloadFiles $resolvedPackage)
        foreach ($item in $payload) {
            $null = Join-SafeChildPath $target $item.Relative
        }
        if (-not $Apply) {
            $preview = [ordered]@{
                Mode                   = 'Preview'
                Action                 = 'Install'
                Plugin                 = 'DLSS5 Neural Filter'
                Destination            = $target
                LegacyProgramDataFound = [bool]$legacyExists
                Files                  = @($payload | Select-Object Relative, SHA256)
            }
            $preview | ConvertTo-Json -Depth 5
            return
        }

        $id = 'Install-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $backupDir = Join-Path $resolvedBackupBase $id
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
        foreach ($helper in @('Restore.ps1', 'Delivery.ps1')) {
            $helperPath = Join-Path $PSScriptRoot $helper
            if (Test-Path -LiteralPath $helperPath -PathType Leaf) {
                Copy-Item -LiteralPath $helperPath -Destination $backupDir
            }
        }
        $entries = @()
        $receiptPath = Join-Path $backupDir 'receipt.json'
        function Write-Receipt([string]$StateName) {
            [ordered]@{
                SchemaVersion   = 1
                State           = $StateName
                Plugin          = 'DLSS5 Neural Filter'
                Destination     = $target
                BackupDirectory = $backupDir
                Entries         = $entries
                CreatedAt       = (Get-Date).ToUniversalTime().ToString('o')
            } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
        }
        Write-Receipt 'Installing'
        $failure = $null
        $state = 'Installed'
        try {
            foreach ($file in $payload) {
                $destFile = Join-SafeChildPath $target $file.Relative
                $originalHash = $null
                if (Test-Path -LiteralPath $destFile -PathType Leaf) {
                    $originalHash = Get-Sha256Upper $destFile
                    $backupFile = Join-SafeChildPath $backupDir ('original\' + $file.Relative)
                    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backupFile)) -Force | Out-Null
                    Copy-Item -LiteralPath $destFile -Destination $backupFile -Force
                    if ((Get-Sha256Upper $backupFile) -ne $originalHash) { throw 'Backup checksum mismatch' }
                }
                $entries += ,[pscustomobject]@{
                    Relative        = $file.Relative
                    OriginalSHA256  = $originalHash
                    InstalledSHA256 = $file.SHA256
                }
                Write-Receipt 'Installing'
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destFile)) -Force | Out-Null
                Copy-Item -LiteralPath $file.Source -Destination $destFile -Force
                if ((Get-Sha256Upper $destFile) -ne $file.SHA256) {
                    throw "Installed checksum mismatch: $($file.Relative)"
                }
            }
        } catch {
            $state = 'Interrupted'
            $failure = $_
        }
        Write-Receipt $state
        if ($state -eq 'Interrupted') { throw $failure }

        $legacyMoved = ''
        if ($doLegacyAction -and $resolvedLegacyDir -and (Test-Path -LiteralPath $resolvedLegacyDir -PathType Container)) {
            $legacyArchiveParent = Join-Path $resolvedBackupBase ('Legacy-ProgramData-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
            New-Item -ItemType Directory -Path $legacyArchiveParent -Force | Out-Null
            $legacyMoved = Join-Path $legacyArchiveParent 'obs-dlss5'
            Copy-Item -LiteralPath $resolvedLegacyDir -Destination $legacyMoved -Recurse -Force
            Remove-Item -LiteralPath $resolvedLegacyDir -Recurse -Force
        }

        Write-Ui ''
        Write-Ui (Msg '安装成功！' 'Installation succeeded!') Green
        Write-Ui (Msg "  安装目录: $target" "  OBS Root: $target")
        Write-Ui (Msg "  写入文件: $($payload.Count) 个（含校验）" "  Files installed: $($payload.Count) (SHA256 verified)")
        Write-Ui (Msg "  备份收据: $receiptPath" "  Backup receipt: $receiptPath")
        if ($legacyMoved) {
            Write-Ui (Msg "  已移出旧版 ProgramData 目录至备份: $legacyMoved" "  Moved legacy ProgramData folder to backup: $legacyMoved") Yellow
        }
        Write-Ui (Msg '现在可以启动 OBS，并在「来源 → 滤镜 → 效果滤镜 → DLSS5 Neural Filter」中添加使用。' 'You can now launch OBS and add Filters -> Effect Filters -> DLSS5 Neural Filter.') Cyan

        if ($NonInteractive -and -not $TranscriptLog) {
            [pscustomobject]@{
                State       = 'Installed'
                Destination = $target
                Receipt     = $receiptPath
                LegacyMoved = $legacyMoved
                FileCount   = $payload.Count
            }
        }
    } else {
        # Uninstall mode
        $ownedFiles = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        if ($target -and -not $legacyOnlyUninstall) {
            foreach ($rel in @(
                'obs-plugins\64bit\obs-dlss5.dll',
                'obs-plugins\64bit\runtime\nvngx_dlssnr.dll',
                'obs-plugins\64bit\runtime\rtx50\nvngx_dlssnr.dll'
            )) {
                $full = Join-SafeChildPath $target $rel
                if (Test-Path -LiteralPath $full -PathType Leaf) { $null = $ownedFiles.Add($full) }
            }
            $pluginDataDir = Join-SafeChildPath $target 'data\obs-plugins\obs-dlss5'
            if (Test-Path -LiteralPath $pluginDataDir -PathType Container) {
                foreach ($f in @(Get-ChildItem -LiteralPath $pluginDataDir -File -Recurse)) {
                    $null = $ownedFiles.Add((Join-SafeChildPath $target (Resolve-FullPath $f.FullName).Substring($target.TrimEnd('\', '/').Length + 1)))
                }
            }
        }

        $plannedRemoved = @($ownedFiles | Sort-Object)
        if (-not $Apply) {
            [ordered]@{
                Mode                   = 'Preview'
                Action                 = 'Uninstall'
                Plugin                 = 'DLSS5 Neural Filter'
                Destination            = $target
                FilesToRemove          = $plannedRemoved
                LegacyProgramDataFound = [bool]$legacyExists
            } | ConvertTo-Json -Depth 5
            return
        }

        $removedCount = 0
        foreach ($file in $plannedRemoved) {
            if (Test-Path -LiteralPath $file -PathType Leaf) {
                Remove-Item -LiteralPath $file -Force
                $removedCount++
            }
        }

        if ($target -and -not $legacyOnlyUninstall) {
            $pruneDirs = @()
            $pluginDataDir = Join-SafeChildPath $target 'data\obs-plugins\obs-dlss5'
            if (Test-Path -LiteralPath $pluginDataDir -PathType Container) {
                $subDirs = @(Get-ChildItem -LiteralPath $pluginDataDir -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending)
                foreach ($sd in $subDirs) { $pruneDirs += $sd.FullName }
                $pruneDirs += $pluginDataDir
            }
            $pruneDirs += (Join-SafeChildPath $target 'obs-plugins\64bit\runtime\rtx50')
            $pruneDirs += (Join-SafeChildPath $target 'obs-plugins\64bit\runtime')
            foreach ($dir in $pruneDirs) {
                if ((Test-Path -LiteralPath $dir -PathType Container) -and @(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $dir -Force
                }
            }
        }

        $legacyRemoved = $false
        if ($doLegacyAction -and $resolvedLegacyDir -and (Test-Path -LiteralPath $resolvedLegacyDir -PathType Container)) {
            Remove-Item -LiteralPath $resolvedLegacyDir -Recurse -Force
            $legacyRemoved = $true
        }

        Write-Ui ''
        Write-Ui (Msg '卸载完成！' 'Uninstall complete!') Green
        if ($target -and -not $legacyOnlyUninstall) {
            Write-Ui (Msg "  已清理目录: $target" "  Target OBS Root: $target")
            Write-Ui (Msg "  移除文件数: $removedCount" "  Removed files: $removedCount")
        }
        if ($legacyRemoved) {
            Write-Ui (Msg "  已清理旧版目录: $resolvedLegacyDir" "  Removed legacy directory: $resolvedLegacyDir") Yellow
        }

        if ($NonInteractive -and -not $TranscriptLog) {
            [pscustomobject]@{
                State         = 'Uninstalled'
                Destination   = $target
                RemovedFiles  = $removedCount
                LegacyRemoved = $legacyRemoved
            }
        }
    }
} catch {
    Write-Ui ''
    Write-Ui (Msg "操作失败: $($_.Exception.Message)" "Operation failed: $($_.Exception.Message)") Red
    if ($TranscriptLog) { exit 1 }
    throw
}
