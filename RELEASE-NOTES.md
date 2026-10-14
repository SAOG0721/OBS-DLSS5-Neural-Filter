# DLSS5 Neural Filter v0.1.0

## 中文

为 OBS 的单个来源添加 DLSSNR 神经画面增强，用于游戏捕获、视频与摄像头的直播和录制。

### 功能

- **1–3 次 NR 处理**：每次独立调节样式、NR 强度、局部色调、局部结构、皮肤结构、自动遮罩和界面修正。
- **效果、颜色与细节控制**：输出混合、整体强度、色度、明度、变暗/变亮贡献，以及色相、暗部、亮部保护和高低频细节。
- **光流与抗闪烁**：支持 AMDOF / NVOF，提供静态、光流、光流增强、低频抗闪烁模式。
- **直观比较**：正常、原图、分屏、差分视图；支持 25%–100% 推理尺寸，保持原来源输出尺寸与透明度。
- **实时预览**：多个预览共享已完成的处理画面，GPU 繁忙时保留最近完成的结果。
- **中英文界面**：按模型、颜色、保护、运动和性能整理参数。
- **附带运行库**：自动查找包内 DLSSNR DLL，默认无需填写路径。

### 安装与部署

1. 下载 **DLSS5-Neural-Filter-0.1.0-Windows-x64.zip**，完整解压并关闭 OBS。
2. **常规 OBS**：右键 `Install.cmd`，选择“以管理员身份运行”。
3. **Steam / 便携版 OBS**：将 OBS 安装根目录拖到 `Install-OBS.cmd` 上。该目录中应包含 `bin\64bit\obs64.exe`。
4. 启动 OBS，进入来源的 **滤镜 → 效果滤镜 → DLSS5 Neural Filter**，运行库路径留空即可。

建议从一次 NR、100% 推理尺寸、关闭光流和抗闪烁开始，用分屏比较后逐步调整。需要降低 GPU 开销时，可减少处理次数或降低推理尺寸。

适用于 Windows x64、OBS Direct3D 11，以及兼容 DLSSNR 的 NVIDIA RTX 显卡与驱动。当前处理 SDR；HDR 保留原图。OBS 兼容性按接口与图形能力判断，不设数字版本硬门禁。

安装工具自动备份旧版，包内 `INSTALL.md` 提供手动安装、更新与回退步骤。

### 下载内容

- **Windows-x64.zip**：插件、DLSSNR 运行库、中英文资源、安装与回退工具。

插件源码采用 **GPL-3.0-only**。第三方组件许可随包附带。

---

## English

Add DLSSNR neural image enhancement to individual OBS sources for streaming and recording game capture, video and camera content.

### Features

- **1–3 NR passes** with independent style, intensity, local tone, local structure, skin structure, auto mask and UI correction.
- **Effect, color and detail controls** for output mix, overall strength, chroma, lightness, darkening/brightening contributions, hue and tonal protection, and low/high-frequency detail.
- **Optical flow and anti-flicker** with AMDOF / NVOF and Static, Flow, Flow+ and Low Frequency modes.
- **Visual comparison** through Normal, Original, Split and Difference views. Inference resolution can be set to 25%–100% while preserving source output size and transparency.
- **Real-time previews** share completed images and retain the latest result while the GPU is busy.
- **Chinese and English controls**, grouped by model, color, protection, motion and performance.
- **Bundled DLSSNR runtime** with automatic discovery; no runtime path is required by default.

### Installation

1. Download **DLSS5-Neural-Filter-0.1.0-Windows-x64.zip**, extract the complete archive and close OBS.
2. **Standard OBS:** right-click `Install.cmd` and choose **Run as administrator**.
3. **Steam / portable OBS:** drag the OBS installation root folder onto `Install-OBS.cmd`. This folder should contain `bin\64bit\obs64.exe`.
4. Start OBS and select **Filters → Effect Filters → DLSS5 Neural Filter** for your source. Leave the runtime path empty.

Start with one NR pass, 100% inference resolution, optical flow off and anti-flicker off. Use Split view to compare changes. Reduce the pass count or inference resolution to lower GPU load.

Requires Windows x64, the OBS Direct3D 11 renderer, and an NVIDIA RTX GPU and driver compatible with DLSSNR. This version processes SDR; HDR passes through. OBS compatibility is based on interfaces and graphics capabilities, with no version-number gate.

The installer backs up the previous version. The included `INSTALL.md` covers manual installation, updates and rollback.

### Downloads

- **Windows-x64.zip:** plugin, DLSSNR runtime, Chinese/English resources, installation and rollback tools.

The plugin source is licensed under **GPL-3.0-only**. Third-party licenses are included.
