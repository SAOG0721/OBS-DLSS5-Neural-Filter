# DLSS5 Neural Filter v0.2.2

## 中文

本版同步 **Magpie Experimental 0.6.13 / 0.6.14** 的 DLSSNR 画质与自适应策略改进，全面重构 OBS 滤镜属性界面与中英文文档，并重构为一键安装/卸载工具链：

- **NVOF 隔帧局部恢复与时域保护**：在 **NVOF + 隔帧** 模式下同步局部历史块搜索（8×8 Tile 4 锚点共享搜索 + 半像素细化）、3×3 静止邻域精确恢复及全分辨率输入引导；抗闪烁 `Temporal` 升级为 3×3 邻域补丁匹配与运动边界保护，减轻完整推理帧与复用帧切换时的亮度与细节跳变。
- **自适应分辨率试档验收与自动回退**：将 D3D12 耗时拆分为可缩放阶段与固定后处理阶段，识别固定瓶颈并拦截无收益降档；降档／升档进入试档验证，无明显收益或过载时通过上一档热备直接恢复原档，并短期封锁反复抖动。
- **OBS 滤镜界面重构（扁平 5 分页）**：
  - 去除嵌套分组框与右列长文本墙，改用顶部「参数分页」切换（`快速调节 / 多层 NR / 色彩与保护 / 性能与抗闪烁 / 运行库与诊断`），全页共享同一单列表单对齐，默认打开「快速调节」一屏即可完成常规调参。
  - `多层 NR` 改用 `NR 次数` + `编辑层级` 单层编辑器，无论 1–3 层均保持精简固定行数；色彩保护与耦合的性能/光流/抗闪烁分别同页集中展示；所有提示信息均使用左标签对齐、按正常／警告／错误分级着色并开启自动换行。
  - `NR 次数` 改用下拉列表防误触，分辨率比例与响应时间补充 `%` / `ms` 后缀，清理非必要的 `?` 图标噪点，新增 `zh-TW` 繁体中文与复制诊断成功提示。
- **全新一键安装 / 卸载与双模式说明**：
  - 统一安装布局到 **OBS 根目录**（常规版 / Steam 版 / 便携版一致），压缩包内置 `Install.cmd` 与 `Uninstall.cmd`：自动发现注册表与所有 Steam 库中的 OBS、按需自动请求管理员权限、覆盖前自动备份、检测并安全清理老版本 `C:\ProgramData\obs-studio\plugins\obs-dlss5` 残留。
  - 保留 `obs-plugins`、`data` 与 `安装说明.txt` 结构，同步支持直接手动复制安装；全面重写中英文 [README.md](README.md) / [README_EN.md](README_EN.md)。

## English

This release syncs the DLSSNR image-quality and adaptive-scaling improvements from **Magpie Experimental 0.6.13 / 0.6.14**, overhauls the OBS filter properties UI and documentation, and replaces the split installer scripts with a unified one-click installer/uninstaller:

- **NVOF alternating-frame recovery and temporal protection**: Syncs 8×8 tile 4-anchor local history recovery with half-pixel refinement, 3×3 stationary neighborhood recovery, and full-resolution color guidance for **NVOF + Alternating frames**, and upgrades `Temporal` to 3×3 patch matching with motion-discontinuity protection to reduce brightness/detail flicker between full and reused frames.
- **Adaptive resolution trial verification and auto-restore**: Splits scalable D3D12 stages from fixed post-processing cost to avoid futile downshifts on fixed bottlenecks, verifies downshift/upshift trials, automatically restores the previous tier from a hot standby when a trial yields no benefit or overloads, and suppresses immediate oscillation.
- **OBS filter UI overhaul (flat 5-section layout)**:
  - Removes nested `QGroupBox` frames and right-column text walls in favor of a top `Section` switcher (`Quick / NR passes / Color & protection / Performance & anti-flicker / Runtime & diagnostics`) sharing one aligned form layout so `Quick` fits cleanly without heavy scrolling.
  - Replaces stacked multi-pass groups with a `NR passes` + `Edit pass` single-pass editor, groups coupled performance/flow/anti-flicker controls together, isolates troubleshooting tools, and aligns left-labelled `OBS_TEXT_INFO` rows with severity coloring and word wrap.
  - Changes `NR passes` to a dropdown, adds `%` and `ms` suffixes, removes redundant `?` icon clutter, adds clipboard-copy confirmation, and adds `zh-TW` localization.
- **Unified one-click installer & documentation overhaul**:
  - Standardizes installation on the **OBS root directory** across Standard, Steam, and Portable builds, bundling `Install.cmd` and `Uninstall.cmd` (auto-detecting OBS in Windows registry and Steam libraries, elevating via UAC only when needed, backing up replaced files, and cleaning up legacy `C:\ProgramData\obs-studio\plugins\obs-dlss5` copies to prevent duplicate loading).
  - Preserves direct manual folder copying (`obs-plugins` + `data`) and rewrites [README.md](README.md), [README_EN.md](README_EN.md), and `安装说明.txt`.

---

# DLSS5 Neural Filter v0.2.1

## 中文

- 模型、推理尺寸、处理层和光流的资源准备及回收移到后台，连续调整只保留最新请求；首个新结果完成后自动切换。
- 精简关闭抗闪烁时的等价后处理、无用历史复制和重复清零，保留画质、透明度与已保存参数。
- 改善连续调参时的像素验证和增强重合成；修复隔帧模式下固定画面周期的出口调节停滞。
- 减少重复诊断开销和无效表单重建；信息刷新避开连续拖动，状态和错误继续及时更新。
- 分发包继续按 `obs-plugins`、`data` 和 `安装说明.txt` 直接复制安装。

## English

- Prepare and retire model, inference-size, pass and optical-flow resources in the background, retaining the latest request and switching after the first new result completes.
- Remove equivalent anti-flicker-off work, unused history copies and redundant clearing while preserving image behavior, alpha and saved settings.
- Improve pixel verification during continuous edits and fix stalled outlet recomposition in alternating mode.
- Reduce diagnostic overhead and unnecessary form rebuilds; defer informational reloads during a drag while retaining state/error updates.
- Keep the copy-to-OBS package layout: `obs-plugins`, `data` and `安装说明.txt`.

---

# DLSS5 Neural Filter v0.2.0

## 中文

本版将喜鹊 0.6.12 的模型、颜色与性能控制带入 OBS 来源滤镜，用于游戏捕获、视频与摄像头的直播和录制。

### 功能

- **模型选择**：自动（DLL 默认）、HT、CC Small FP8，所有 NR 层共用选择；默认 HT。实际模型支持取决于 GPU 与所选 DLL。
- **中间调与颜色保护**：新增中间调强度和全域色度保护，改善低频／高频残差、亮部保护与最终颜色限制的处理顺序。
- **自适应分辨率**：开启调整推理尺寸后，可选择自适应，按 OBS 目标帧率以 5 个百分点切档；手动仍为 25%–100%，切回时恢复保存的比例。
- **隔帧增强**：可每 2／3／4 个独立 OBS 视频周期更新完整 NR，中间帧继续使用当前画面，叠加历史增强。支持原位复用和 AMDOF／NVOF 运动重投影。
- **时域与预览**：改进零修正、历史失效和 NVOF 输入交接；额外预览复用同周期结果。面板显示完整更新、轻量合成、增强年龄及当前推理比例。
- **保留常用控制**：1–3 次 NR、每层模型样式与强度、颜色和细节、四种抗闪烁、原图／分屏／差分视图，以及来源透明度。
- **更新运行库**：主包默认附带 310.9.16 社区兼容版，同时包含 NVIDIA 原版 RTX 50 选项。
- **参数排版**：推理与性能前移，进阶入口置顶，中间调紧跟明度；详细统计作为进阶快照显示，已开启的修正诊断保留明确入口。

隔帧按 OBS 视频周期计数：30 FPS 来源放入 60 FPS 画布时，使用 60 FPS 画布周期。关闭 NR 光流时，运动重投影回退为原位复用；光流型抗闪烁仍可独立使用 AMDOF。较低推理比例和较长更新间隔会减少处理量，也会影响细节和运动中的增强表现。

### 安装与部署

1. 下载 **DLSS5-Neural-Filter-0.2.0-Windows-x64.zip**，完整解压并关闭 OBS。
2. 将包内 **obs-plugins** 和 **data** 两个文件夹复制到 OBS 安装根目录，合并并覆盖同名插件文件。该目录中应包含 `bin\64bit\obs64.exe`。
3. 启动 OBS，进入来源的 **滤镜 → 效果滤镜 → DLSS5 Neural Filter**，运行库路径留空即可。

新建滤镜默认 HT、一次 NR、逐帧、手动 100%，光流和抗闪烁关闭。旧场景沿用已保存设置，缺失的新控制采用保持原有行为的默认值。先用分屏比较，再按需要开启自适应或隔帧。

运行库留空使用兼容版。RTX 50 用户需要选择 NVIDIA 原版时，在运行库字段选取安装目录中的 `runtime/rtx50/nvngx_dlssnr.dll`。结构设置更改由插件自动准备资源；颜色与残差数值可实时调整。

适用于 Windows x64、OBS Direct3D 11，以及兼容 DLSSNR 的 NVIDIA RTX 显卡与驱动。当前处理 SDR；HDR 保留原图。OBS 兼容性按接口与图形能力判断，不设数字版本硬门禁。

包内 `安装说明.txt` 列出安装步骤和目录位置。更新前可先备份插件文件；旧版若位于 ProgramData 插件目录，先按说明移走，避免重复加载。

### 下载内容

- **Windows-x64.zip**：与 OBS 安装目录对应的 `obs-plugins`、`data` 两个文件夹，以及 `安装说明.txt`；包含插件、两种 DLSSNR 运行库、中英文资源和许可证。

插件源码采用 **GPL-3.0-only**。第三方组件许可随包附带。

---

## English

This version brings Magpie 0.6.12 model, color and performance controls to OBS source filters for streaming and recording game capture, video and camera content.

### Features

- **Model presets:** Automatic (DLL Default), HT and CC Small FP8, shared by all passes; HT is the default. Support depends on the GPU and selected DLL.
- **Midtones and color protection:** add Midtone Strength and global Chroma Protection, and improve the processing order for low/high-frequency residuals, highlight protection and final color limiting.
- **Adaptive resolution:** enable inference scaling and choose Adaptive to adjust in five-percentage-point steps against the OBS target frame rate. Manual remains 25%–100%, and its saved percentage is restored when selected again.
- **Alternating enhancement:** update full NR every 2 / 3 / 4 distinct OBS video cycles. Intervening frames combine the current picture with historical enhancement in place or through AMDOF / NVOF motion reprojection.
- **Temporal output and previews:** improve zero-correction, invalid-history handling and NVOF input handoff. Extra previews reuse the cycle's result. The panel reports full updates, lightweight composites, enhancement age and the active inference scale.
- **Existing controls retained:** 1–3 NR passes, independent pass styles and strengths, color/detail controls, four anti-flicker modes, Original / Split / Difference views and source transparency.
- **Updated runtimes:** the main package includes 310.9.16 community compatibility by default and an NVIDIA original RTX 50 option.
- **Parameter layout:** inference and performance appear earlier, advanced visibility is at the top, and midtones follow lightness. Detailed statistics are an advanced snapshot, and active correction diagnostics remain discoverable.

Update intervals count OBS video cycles: a 30 FPS source on a 60 FPS canvas uses the 60 FPS canvas cadence. Disabling NR optical flow changes reprojection to in-place reuse; motion-based anti-flicker can still use AMDOF separately. Lower inference scales and longer update intervals reduce work and can affect detail and enhancement in motion.

### Installation

1. Download **DLSS5-Neural-Filter-0.2.0-Windows-x64.zip**, extract the complete archive and close OBS.
2. Copy the **obs-plugins** and **data** folders into the OBS root, merging folders and replacing existing plugin files. The root should contain `bin\64bit\obs64.exe`.
3. Start OBS and select **Filters → Effect Filters → DLSS5 Neural Filter** for your source. Leave the runtime path empty.

New filters default to HT, one pass, Every Frame, Manual 100%, optical flow off and anti-flicker off. Existing scenes retain saved settings; missing new controls use defaults that preserve their previous behavior. Begin with Split view, then enable Adaptive or Alternating Frames as needed.

An empty runtime field uses the compatibility build. To use the NVIDIA original on RTX 50, select the installed `runtime/rtx50/nvngx_dlssnr.dll`. The plugin prepares resources for structural setting changes automatically; color and residual numeric controls apply live.

Requires Windows x64, the OBS Direct3D 11 renderer, and an NVIDIA RTX GPU and driver compatible with DLSSNR. This version processes SDR; HDR passes through. OBS compatibility is based on interfaces and graphics capabilities, with no version-number gate.

The included `安装说明.txt` shows the steps and directory layout. Back up existing plugin files before updating. For an older ProgramData installation, move the old plugin folder out as described in the guide to avoid duplicate loading.

### Downloads

- **Windows-x64.zip:** the `obs-plugins` and `data` folders matching OBS's installation layout, plus `安装说明.txt`; includes the plugin, both DLSSNR runtime options, Chinese/English resources and licenses.

The plugin source is licensed under **GPL-3.0-only**. Third-party licenses are included.
