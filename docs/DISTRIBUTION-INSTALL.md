# 安装与使用 / Installation

## 中文

从 [Releases](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) 下载 `DLSS5-Neural-Filter-0.1.0-Windows-x64.zip`，完整解压到本地文件夹，关闭 OBS。

### 常规 OBS

右键 **Install.cmd → 以管理员身份运行**。默认安装到：

```text
C:\ProgramData\obs-studio\plugins\obs-dlss5
```

### Steam / 便携版 OBS

将 **OBS 安装根目录**拖到 **Install-OBS.cmd** 上。安装根目录内应包含 `bin\64bit\obs64.exe`。

需要指定路径时，在解压目录打开 PowerShell：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-OBS.ps1 -ObsRoot 'D:\SteamLibrary\steamapps\common\OBS Studio' -Apply
```

替换为实际 OBS 路径；目录需要管理员写入权限时，使用管理员 PowerShell。

### 手动安装

将包内 `obs-dlss5/bin/64bit` 的内容复制到 OBS 的 `obs-plugins/64bit`，将 `obs-dlss5/data` 的内容复制到 `data/obs-plugins/obs-dlss5`。最终目录为：

```text
OBS 安装根目录/
  obs-plugins/64bit/obs-dlss5.dll
  obs-plugins/64bit/runtime/nvngx_dlssnr.dll
  data/obs-plugins/obs-dlss5/...
```

更新时沿用原来的安装方式。手动覆盖前请自行保存旧文件；使用安装脚本会自动备份。

### 添加滤镜

启动 OBS，选择来源 → **滤镜 → 效果滤镜 → DLSS5 Neural Filter**。保持“DLSSNR 运行库 DLL”为空，插件会自动找到附带的 `runtime/nvngx_dlssnr.dll`。

先使用一次 NR、100% 推理尺寸，关闭光流和抗闪烁。使用“分屏”比较效果，再调节混合、强度、模型参数和颜色。需要降低开销时，可减少 NR 次数或降低推理比例；需要稳定运动画面时，可选择光流和抗闪烁模式。

若要使用另一份兼容 DLL，可在滤镜的运行库字段选择完整路径。处理状态和实际路径显示在滤镜面板中，“重试初始化”与“复制诊断”可用于排查。

### 恢复旧版或卸载

安装脚本在覆盖前自动备份，并输出 `Receipt` 路径。默认备份目录：

```text
%LOCALAPPDATA%\Dlss5NeuralFilter\Backups
```

关闭 OBS，使用对应的安装收据回退：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Restore.ps1 -Receipt '备份目录\receipt.json' -Apply
```

也可使用备份目录内保存的 `Restore.ps1`。首次安装的回退会移除新增文件；覆盖安装的回退会恢复旧文件。自行修改过的文件会提示冲突，可先去掉 `-Apply` 查看回退计划。

### 文件校验与命令行

```powershell
# 核对包内文件
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Verify-Package.ps1

# 常规安装；不带 -Apply 时只显示计划
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Apply
```

## English

Download `DLSS5-Neural-Filter-0.1.0-Windows-x64.zip` from [Releases](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases), extract the complete archive and close OBS.

### Standard OBS

Right-click **Install.cmd → Run as administrator**. The default destination is:

```text
C:\ProgramData\obs-studio\plugins\obs-dlss5
```

### Steam / portable OBS

Drag the **OBS installation root folder** onto **Install-OBS.cmd**. The folder should contain `bin\64bit\obs64.exe`.

Alternatively, open PowerShell in the extracted folder:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-OBS.ps1 -ObsRoot 'D:\SteamLibrary\steamapps\common\OBS Studio' -Apply
```

Replace the example with your actual path. Use an administrator PowerShell if the destination requires elevated write access.

### Manual installation

Copy the contents of `obs-dlss5/bin/64bit` into OBS's `obs-plugins/64bit`, including the private `runtime` folder. Copy the contents of `obs-dlss5/data` into `data/obs-plugins/obs-dlss5`.

For an update, use the same installation layout as before. Save the old files before a manual replacement; the installation scripts create backups automatically.

### Add the filter

Select a source in OBS, then **Filters → Effect Filters → DLSS5 Neural Filter**. Leave **DLSSNR runtime DLL** empty to load the bundled `runtime/nvngx_dlssnr.dll`.

Start with one NR pass, 100% inference resolution, optical flow off and anti-flicker off. Use Split view to compare changes, then adjust mix, strength, model and color controls. Reduce pass count or inference resolution to lower GPU load, or enable flow and anti-flicker to stabilize motion.

To use another compatible runtime, select its full path in the filter. The panel shows processing status and the resolved path, with **Retry initialization** and **Copy diagnostics** actions.

### Rollback or uninstall

The installer prints a `Receipt` path and keeps backups in `%LOCALAPPDATA%\Dlss5NeuralFilter\Backups`.

Close OBS and restore using the receipt from the relevant installation:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Restore.ps1 -Receipt 'backup folder\receipt.json' -Apply
```

You can also use the copy of `Restore.ps1` inside the backup folder. Rolling back a first installation removes the added files; rolling back an update restores the previous files. Omit `-Apply` to preview the plan, including any conflicts with files you changed after installation.

### Verification and command line

```powershell
# Verify package contents
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Verify-Package.ps1

# Standard installation; omit -Apply for a preview
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Apply
```
