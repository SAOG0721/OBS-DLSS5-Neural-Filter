DLSS5 Neural Filter 0.2.2 — 安装与卸载说明
================================================

开始前请先完全关闭 OBS Studio（obs64.exe）。

------------------------------------------------
方式一：脚本一键安装 / 卸载（推荐）
------------------------------------------------
1. 完整解压当前压缩包（不要直接在压缩软件里双击运行）。
2. 双击运行 Install.cmd：
   - 会自动识别本机安装的常规版与 Steam 版 OBS；
   - 如果使用的是便携版 OBS，可直接把 OBS 根目录拖到 Install.cmd 上，或在弹窗中手动选择；
   - 写入系统盘目录时会自动请求管理员权限，并在覆盖前备份旧文件至：
     %LOCALAPPDATA%\Dlss5NeuralFilter\Backups
3. 如需卸载：关闭 OBS 后双击运行 Uninstall.cmd，只会移除本插件相关文件，不影响 OBS 或其他插件。

------------------------------------------------
方式二：手动复制安装
------------------------------------------------
1. 找到 OBS 安装根目录（包含 bin\64bit\obs64.exe 的文件夹；常规版、Steam 版与便携版目录结构相同）。
2. 将压缩包中的 obs-plugins 与 data 两个文件夹复制到该目录，合并文件夹并覆盖同名插件文件。
3. 启动 OBS，选择「来源 → 滤镜 → 效果滤镜 → DLSS5 Neural Filter」，运行库路径留空即可。

安装后的关键目录结构：
  OBS 安装根目录/
    bin/64bit/obs64.exe
    obs-plugins/64bit/
      obs-dlss5.dll
      runtime/nvngx_dlssnr.dll
      runtime/rtx50/nvngx_dlssnr.dll
    data/obs-plugins/obs-dlss5/
      locale/ (zh-CN.ini, zh-TW.ini, en-US.ini)
      licenses/
      neural.hlsl
      output.effect
      LICENSE
      THIRD_PARTY_NOTICES.md

------------------------------------------------
注意事项
------------------------------------------------
- 旧版迁移：若之前将老版本安装在 C:\ProgramData\obs-studio\plugins\obs-dlss5，
  Install.cmd 会提示并自动备份移出；若手动安装，请先手动备份并移走该目录，避免 OBS 重复加载两个插件。
- RTX 50 显卡选项：默认运行库路径留空即可使用内置 310.9.16 社区兼容版。
  如需切换随附的 NVIDIA 原版运行库，可在滤镜「运行库与诊断」分页选择：
  obs-plugins\64bit\runtime\rtx50\nvngx_dlssnr.dll

================================================
English Summary
================================================
Close OBS completely before installing or uninstalling.
- One-Click (Recommended): Extract the entire archive and double-click Install.cmd
  (auto-detects Standard & Steam OBS; drag your portable OBS folder onto Install.cmd or browse in the dialog).
  Double-click Uninstall.cmd to cleanly remove the plugin.
- Manual Install: Copy the extracted "obs-plugins" and "data" folders into your OBS root folder
  (the directory containing bin\64bit\obs64.exe), merging folders and replacing existing plugin files.
- Legacy ProgramData cleanup: If an older copy exists at C:\ProgramData\obs-studio\plugins\obs-dlss5,
  Install.cmd backs it up and moves it out automatically (move it out manually if copying by hand).
- Launch OBS, add Filters -> Effect Filters -> DLSS5 Neural Filter, and leave the runtime DLL path empty.
