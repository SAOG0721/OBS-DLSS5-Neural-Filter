# DLSS5 Neural Filter for OBS

[English](README_EN.md) · [下载](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) · [安装说明](docs/DISTRIBUTION-INSTALL.md)

将 DLSSNR 神经画面增强作为 **OBS 单来源效果滤镜**，用于游戏捕获、视频、摄像头等来源的直播与录制。

这是 [Magpie Experimental（大力喜鹊）](https://github.com/SAOG0721/Magpie) 作者开发的衍生插件，把神经画面增强与调节功能带入 OBS 的来源滤镜工作流。

## 主要功能

| 功能 | 说明 |
| --- | --- |
| 多次 NR 处理 | 支持 1–3 次处理，每次独立设置样式、NR 强度、局部色调、局部结构、皮肤结构、自动遮罩与界面修正 |
| 效果与颜色调节 | 输出混合、整体强度、色度、明度，以及变暗和变亮贡献 |
| 保护与细节 | 色相、暗部、亮部保护，过度修正抑制，以及低频和高频强度 |
| 运动与抗闪烁 | AMDOF / NVOF 光流；静态、光流、光流增强、低频四种抗闪烁模式，可独立关闭 |
| 画面对比 | 正常、原图、分屏、差分视图，便于比较和调整效果 |
| 推理尺寸 | 支持 25%–100% 推理尺寸比例，输出保持来源尺寸 |
| 实时输出 | 多个预览复用已完成的结果；GPU 繁忙时保留最近完成画面，恢复后继续更新 |
| 开箱即用 | 分发包附带 DLSSNR 运行库并自动查找，提供中英文界面与安装、回退工具 |

## 使用环境

- Windows x64，OBS Studio 使用 Direct3D 11 渲染。
- 支持附带 DLSSNR 运行库的 NVIDIA RTX 显卡与驱动。
- 当前处理 SDR 画面，HDR 来源和 HDR 画布保留原图。
- 来源最大为 3840 × 2160，实际推理尺寸范围为 256 × 144 至 3840 × 2160。

已验证配置为 OBS 32.2.2、RTX 5070 Ti、616.92 驱动。插件根据所需接口与图形能力加载，没有 OBS 数字版本硬门禁。

## 安装

从 [Releases](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) 下载 **Windows-x64.zip**，完整解压并关闭 OBS。已安装本插件时，继续使用原来的安装方式。

### 常规 OBS

右键解压目录中的 **Install.cmd → 以管理员身份运行**。

默认安装位置为 `C:\ProgramData\obs-studio\plugins\obs-dlss5`。

### Steam / 便携版 OBS

把 **OBS 安装根目录**拖到 **Install-OBS.cmd** 上。所选目录中应有 `bin\64bit\obs64.exe`。

也可在解压目录打开 PowerShell，使用实际安装路径执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-OBS.ps1 -ObsRoot 'D:\SteamLibrary\steamapps\common\OBS Studio' -Apply
```

详细步骤、手动安装和回退方法见[安装说明](docs/DISTRIBUTION-INSTALL.md)。

## 添加滤镜与上手设置

1. 启动 OBS，选中需要处理的来源，打开 **滤镜 → 效果滤镜 → DLSS5 Neural Filter**。
2. **DLSSNR 运行库 DLL** 路径留空，插件自动使用附带文件。
3. 先用默认的一次 NR、100% 推理尺寸、关闭光流与抗闪烁开始。
4. 切换到“分屏”观察变化，再调整输出混合、整体强度与每次 NR 的参数。
5. 需要运动稳定时选择光流和抗闪烁；需要降低开销时，减少处理次数或降低推理尺寸比例。

“显示进阶控制”可展开保护、细节等调节。滤镜保留来源透明度，已有 OBS 场景中的参数会随场景保存。

## 更新、恢复与反馈

安装工具会在覆盖前备份旧文件，并输出回退收据路径。默认备份目录为 `%LOCALAPPDATA%\Dlss5NeuralFilter\Backups`。恢复方法见[安装说明](docs/DISTRIBUTION-INSTALL.md#恢复旧版或卸载)。

遇到处理异常时，可在滤镜中点击“重试初始化”或“复制诊断”，并在 [Issues](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/issues) 提供诊断信息、OBS 版本、显卡和复现步骤。

## 源码与许可证

构建依赖和命令见 [BUILDING.md](docs/BUILDING.md)。Release 同时提供配套源码 ZIP 与 SHA256 校验文件。

插件源码采用 **GNU GPL v3（GPL-3.0-only）**，见 [LICENSE](LICENSE)。第三方组件许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
