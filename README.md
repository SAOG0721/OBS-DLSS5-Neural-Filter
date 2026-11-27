<div align="center">

# DLSS5 Neural Filter for OBS

将 **DLSSNR** 神经画质增强带入 OBS 单来源效果滤镜，适用于游戏捕获、视频与摄像头直播/录制。

[![Release](https://img.shields.io/github/v/release/SAOG0721/OBS-DLSS5-Neural-Filter?label=Release&color=2ea44f)](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases/latest)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078d4)](#运行环境)
[![OBS](https://img.shields.io/badge/OBS-Direct3D%2011%20%C2%B7%20%E6%97%A0%E7%89%88%E6%9C%AC%E9%97%A8%E7%A6%81-302E31)](#运行环境)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue)](LICENSE)
[![Origin](https://img.shields.io/badge/Based%20on-Magpie%20Experimental-purple)](https://github.com/SAOG0721/Magpie)

[English](README_EN.md) · [下载 Release](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) · [安装与卸载](#安装与卸载) · [快速上手](#快速上手) · [更新日志](RELEASE-NOTES.md) · [问题反馈](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/issues)

</div>

> 本插件是 [Magpie Experimental（大力喜鹊）](https://github.com/SAOG0721/Magpie) 作者开发的衍生作品，将喜鹊的 DLSSNR 神经画质增强、自适应分辨率、光流隔帧与抗闪烁控制集成到 OBS Studio 滤镜工作流中。

---

## 核心特性

- **1–3 层神经画质增强**：支持 `自动`、`HT`（默认）与 `CC Small FP8` 模型预设，每层独立调节样式、强度、局部色调/结构、皮肤与 UI 修正，完整保留来源 Alpha 透明通道。
- **精细色彩与边缘保护**：提供输出混合、整体强度、色度、明度、中间调、暗部/亮部贡献，以及色相、全域色度、暗部、亮部、过修正抑制与低/高频残差调节。
- **高帧率性能方案**：支持 **25%–100% 手动或自适应推理分辨率**（含试档验证与固定瓶颈保护）、**每 2 / 3 / 4 帧隔帧增强**，以及 **AMDOF / NVOF 局部历史恢复** 与四种抗闪烁模式。
- **扁平 5 分页界面**：支持 **原图 / 分屏 / 差分** 实时对比视图，内置**简体中文 / 繁體中文 / English** 界面，切换页签不中断 GPU 处理。
- **开箱即用**：分发包已内置 `310.9.16` 社区兼容运行库与 RTX 50 原版选项，支持 **一键安装/卸载脚本** 或 **直接复制目录** 两种部署方式。

---

## 运行环境

- **系统与宿主**：Windows 10 / 11 x64；OBS Studio 使用 **Direct3D 11** 渲染（支持常规安装版、Steam 版与便携版）。
- **显卡与驱动**：支持 DLSSNR 的 NVIDIA RTX 显卡及对应驱动。
- **画面范围**：当前处理 **SDR** 画面（HDR 来源与 HDR 画布保持原图直通）；来源最大 `3840 × 2160`，推理尺寸范围 `256 × 144` 至 `3840 × 2160`。
- **兼容性说明**：插件按图形能力与所需导出接口动态加载，**不设 OBS 数字版本硬门禁**；已验证配置为 `OBS 32.2.2 / 30.2.3`、`RTX 5070 Ti`、驱动 `32.0.16.2140`。

---

## 安装与卸载

从 [Releases](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) 下载最新版 **`DLSS5-Neural-Filter-x.x.x-Windows-x64.zip`** 并**完整解压**，安装或卸载前请先**关闭 OBS**。

### 方式一：脚本一键安装（推荐）

1. 解压后，直接双击运行目录中的 **`Install.cmd`**。
2. 脚本会自动扫描注册表与 Steam 库，定位你本机的 **常规版 / Steam 版 OBS**（若安装多个 OBS 可按编号选择；**便携版**可直接把 OBS 根目录拖到 `Install.cmd` 上，或在弹窗中选择）。
3. 写入系统盘目录时脚本会自动请求管理员权限（UAC），并在覆盖前自动备份旧文件。
4. **卸载**：关闭 OBS 后双击运行 **`Uninstall.cmd`**，即可精准移除本插件与资源文件，不影响 OBS 本身及其他插件。

### 方式二：手动复制安装

1. 完整解压下载的 ZIP 包，关闭 OBS。
2. 找到你的 **OBS 安装根目录**（该目录下应包含 `bin\64bit\obs64.exe`；常规版通常为 `C:\Program Files\obs-studio`，Steam 版与便携版结构相同）。
3. 将压缩包内的 **`obs-plugins`** 与 **`data`** 两个文件夹复制到 **OBS 安装根目录**，提示合并文件夹与覆盖同名文件时选择确认。
4. 启动 OBS 即可使用。

<details>
<summary>展开查看：安装后的目录结构</summary>

```text
OBS 安装根目录/
├── bin/64bit/obs64.exe            # OBS 主程序（无需改动）
├── obs-plugins/64bit/
│   ├── obs-dlss5.dll              # 插件主模块
│   └── runtime/
│       ├── nvngx_dlssnr.dll       # 默认 310.9.16 社区兼容运行库
│       └── rtx50/nvngx_dlssnr.dll # 可选 NVIDIA 原版 RTX 50 运行库
└── data/obs-plugins/obs-dlss5/
    ├── locale/                    # zh-CN / zh-TW / en-US
    ├── licenses/                  # 第三方组件许可
    ├── neural.hlsl
    ├── output.effect
    ├── LICENSE
    └── THIRD_PARTY_NOTICES.md
```

</details>

<details>
<summary>展开查看：旧版 ProgramData 迁移与回退旧版本</summary>

- **避免重复加载**：早期版本曾安装在 `C:\ProgramData\obs-studio\plugins\obs-dlss5`。新版 **`Install.cmd`** 检测到该目录时会自动询问并备份移出；若采用手动安装，请先备份并移走该目录后再启动 OBS，否则会加载两份冲突插件。
- **备份与回退**：`Install.cmd` 覆盖旧文件前会在 `%LOCALAPPDATA%\Dlss5NeuralFilter\Backups` 生成备份与 `receipt.json` 收据，可随时按备份内原目录还原，或使用源码仓中的 `scripts/Restore.ps1 -Receipt "<receipt.json 路径>" -Apply` 一键回退。

</details>

<details>
<summary>展开查看：RTX 50 显卡切换 NVIDIA 原版运行库 DLL</summary>

默认情况下，滤镜中的 **DLSSNR 运行库 DLL** 保持留空即可（自动使用随包附带的 `310.9.16` 兼容运行库）。

如果你使用 RTX 50 系列显卡并希望切换为包内附带的 NVIDIA 原版运行库，请在 **运行库与诊断** 分页的运行库路径中选择已安装目录下的：

```text
obs-plugins\64bit\runtime\rtx50\nvngx_dlssnr.dll
```

</details>

---

## 快速上手

1. 启动 OBS，右键需要增强的来源 → **滤镜 → 效果滤镜 → DLSS5 Neural Filter**。
2. 保持 **快速调节** 页默认的 `HT` 模型、1 次 NR、逐帧与 `100%` 推理比例，将 **输出视图** 切为 **分屏**，一边对比画面一边调节 **输出混合**、**整体强度** 与 **NR 强度**。
3. 当 4K 或高帧率场景需要降低显卡负载时，切换到 **性能与抗闪烁** 分页：
   - 开启 **调整推理尺寸** 并设为 **自适应**，让插件按 OBS 目标帧率自动升降分辨率比例；
   - 或将 **处理周期** 设为 **每 2 / 3 帧**，并搭配 **NVOF** 光流与 **运动重投影**，减少完整推理开销同时抑制闪烁。

| 参数分页 | 包含的核心设置 |
| --- | --- |
| **快速调节** | 输出视图（正常 / 原图 / 分屏 / 差分）、输出混合、整体强度、模型预设与第 1 层常用调节 |
| **多层 NR** | 串联层数（1–3 层）与单层切换编辑器（样式、NR 强度、局部色调/结构、皮肤与 UI 修正） |
| **色彩与保护** | 色度、明度、中间调、变暗/变亮贡献、色相/暗部/亮部/全域色度保护、低高频细节 |
| **性能与抗闪烁** | 自适应/手动推理尺寸与响应时间、隔帧周期与复用模式、AMDOF / NVOF 光流与抗闪烁模式 |
| **运行库与诊断** | DLSSNR 运行库路径、GPU 处理统计、修正诊断视图、刷新统计、复制诊断与重试初始化 |

<details>
<summary>展开查看：进阶机制说明（隔帧计数与后台资源准备）</summary>

- **隔帧按 OBS 视频周期计数**：例如 30 FPS 来源放在 60 FPS 画布时，隔帧以 60 FPS 画布周期步进；同一来源的额外预览视图直接复用本周期结果，不会加速跳过推理周期。
- **后台异步切换**：修改模型预设、串联层数、推理尺寸或光流模式时，插件会在后台线程/资源队列中准备与回收资源并合并连续操作，期间继续显示当前可用画面，就绪后自动平滑切换。

</details>

---

## 常见问题与反馈

- **滤镜列表里找不到 `DLSS5 Neural Filter`？**
  - 确认文件位于 **OBS 根目录** 的 `obs-plugins\64bit\obs-dlss5.dll` 与 `data\obs-plugins\obs-dlss5\`，且解压时没有多嵌套一层同名文件夹。
  - 确认 OBS 设置 → 高级 → 渲染器为 **Direct3D 11**（仅支持 D3D11）。
- **报错或效果异常如何反馈？**
  - 在滤镜的 **运行库与诊断** 分页点击 **复制诊断**（必要时也可点 **重试初始化**），前往 [Issues](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/issues) 附上诊断文本、OBS 版本、显卡型号与复现步骤。

---

## 源码构建与许可证

- **构建文档**：依赖配置与 CMake 编译命令见 [BUILDING.md](docs/BUILDING.md)。
- **安装说明（压缩包同步版）**：见 [DISTRIBUTION-INSTALL.md](docs/DISTRIBUTION-INSTALL.md)。
- **开源许可**：本插件源码以 **GNU GPL v3（GPL-3.0-only）** 发布，详见 [LICENSE](LICENSE)；第三方组件及随附运行时许可说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
