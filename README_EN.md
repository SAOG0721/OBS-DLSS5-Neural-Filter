<div align="center">

# DLSS5 Neural Filter for OBS

Bring **DLSSNR** neural image enhancement to OBS Studio as a per-source effect filter for game capture, video, and camera streams or recordings.

[![Release](https://img.shields.io/github/v/release/SAOG0721/OBS-DLSS5-Neural-Filter?label=Release&color=2ea44f)](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases/latest)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078d4)](#requirements)
[![OBS](https://img.shields.io/badge/OBS-Direct3D%2011%20%C2%B7%20No%20Version%20Gate-302E31)](#requirements)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue)](LICENSE)
[![Origin](https://img.shields.io/badge/Based%20on-Magpie%20Experimental-purple)](https://github.com/SAOG0721/Magpie)

[简体中文](README.md) · [Downloads](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) · [Installation](#installation--uninstall) · [Quick Start](#quick-start) · [Release Notes](RELEASE-NOTES.md) · [Issues](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/issues)

</div>

> A spin-off plugin by the author of [Magpie Experimental (大力喜鹊)](https://github.com/SAOG0721/Magpie), bringing its DLSSNR image-enhancement, adaptive inference scaling, optical-flow alternating updates, and anti-flicker controls into OBS Studio.

---

## Highlights

- **1–3 Pass Neural Enhancement**: Supports `Auto`, `HT` (default), and `CC Small FP8` model presets with independent per-pass style, intensity, local tone/structure, skin, and UI correction while preserving source alpha transparency.
- **Color & Detail Protection**: Output mix, overall strength, chroma, lightness, midtone, darkening/brightening balance, hue/chroma/dark/highlight protection, excess-correction suppression, and low/high-frequency detail controls.
- **High-FPS Performance Controls**: **25%–100% Manual or Adaptive inference scaling** (with trial verification and fixed-bottleneck guard), **2 / 3 / 4-frame alternating updates**, and **AMDOF / NVOF local history recovery** with four anti-flicker modes.
- **Flat 5-Section UI**: Single-pass editor, **Original / Split / Difference** comparison views, and **English / 简体中文 / 繁體中文** localization without interrupting GPU processing when switching sections.
- **Ready to Run**: Bundles the `310.9.16` community compatibility runtime and an NVIDIA original RTX 50 option, with both a **one-click installer/uninstaller** and **manual folder copy** in the same release archive.

---

## Requirements

- **OS & Host**: Windows 10 / 11 x64 with OBS Studio running the **Direct3D 11** renderer (Standard, Steam, and Portable builds supported).
- **GPU & Driver**: An NVIDIA RTX GPU and driver compatible with DLSSNR.
- **Signal Limits**: Processes **SDR** frames (HDR sources and HDR canvases pass through untouched); maximum source `3840 × 2160`, inference bounds `256 × 144` to `3840 × 2160`.
- **Compatibility**: Loaded dynamically by required OBS exports and graphics capabilities with **no hard OBS version gate**; verified on `OBS 32.2.2 / 30.2.3`, `RTX 5070 Ti`, driver `32.0.16.2140`.

---

## Installation & Uninstall

Download the latest **`DLSS5-Neural-Filter-x.x.x-Windows-x64.zip`** from [Releases](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) and **extract the entire archive**. Close OBS before installing or uninstalling.

### Option 1: One-Click Installer Script (Recommended)

1. Double-click **`Install.cmd`** in the extracted folder.
2. It automatically scans the Windows registry and Steam libraries for **Standard and Steam OBS** installations (if multiple installs are found, choose by number; for **Portable OBS**, drag your OBS root folder onto `Install.cmd` or pick it in the folder dialog).
3. If the target directory requires elevation (e.g. `Program Files`), UAC is requested automatically and existing plugin files are backed up before replacement.
4. **Uninstall**: Close OBS and double-click **`Uninstall.cmd`** to remove only this plugin's files without touching OBS or other plugins.

### Option 2: Manual Copy

1. Extract the downloaded ZIP completely and close OBS.
2. Locate your **OBS installation root** (the folder containing `bin\64bit\obs64.exe`, e.g. `C:\Program Files\obs-studio` or your Steam/portable OBS folder).
3. Copy the extracted **`obs-plugins`** and **`data`** folders into the **OBS installation root**, confirming folder merge and file replacement.
4. Start OBS.

<details>
<summary>Expand: Installed directory layout</summary>

```text
OBS Installation Root/
├── bin/64bit/obs64.exe            # OBS executable (unchanged)
├── obs-plugins/64bit/
│   ├── obs-dlss5.dll              # Main OBS filter plugin
│   └── runtime/
│       ├── nvngx_dlssnr.dll       # Default 310.9.16 compatibility runtime
│       └── rtx50/nvngx_dlssnr.dll # Optional NVIDIA original RTX 50 runtime
└── data/obs-plugins/obs-dlss5/
    ├── locale/                    # en-US / zh-CN / zh-TW
    ├── licenses/                  # Third-party licenses
    ├── neural.hlsl
    ├── output.effect
    ├── LICENSE
    └── THIRD_PARTY_NOTICES.md
```

</details>

<details>
<summary>Expand: Legacy ProgramData migration & rollback</summary>

- **Avoid duplicate loading**: Older releases could be installed under `C:\ProgramData\obs-studio\plugins\obs-dlss5`. `Install.cmd` detects this folder and offers to back it up and move it aside automatically. If installing manually, move that legacy folder out before launching OBS.
- **Backups & rollback**: Before overwriting files, `Install.cmd` stores backups and `receipt.json` under `%LOCALAPPDATA%\Dlss5NeuralFilter\Backups`, allowing manual restoration or rollback via `scripts/Restore.ps1 -Receipt "<receipt.json>" -Apply` from the source repository.

</details>

<details>
<summary>Expand: Switching to the NVIDIA original runtime on RTX 50</summary>

Leave **DLSSNR runtime DLL** empty by default to use the bundled `310.9.16` compatibility build. To try the NVIDIA original runtime on RTX 50 GPUs, browse to the installed path in the **Runtime & diagnostics** section:

```text
obs-plugins\64bit\runtime\rtx50\nvngx_dlssnr.dll
```

</details>

---

## Quick Start

1. Launch OBS, select a source, and open **Filters → Effect Filters → DLSS5 Neural Filter**.
2. Start with the **Quick** section defaults (`HT`, 1 NR pass, Every Frame, `100%` inference), set **Output view** to **Split**, and dial in **Output mix**, **Overall strength**, and pass controls.
3. To reduce GPU load on 4K or high-FPS captures, open **Performance & anti-flicker**:
   - Enable **Inference scaling** and choose **Adaptive** so inference resolution adjusts automatically to OBS frame-rate headroom; or
   - Set **Update cadence** to **Every 2 / 3 frames** with **NVOF** optical flow and **Motion reprojection** to amortize full NR inference while controlling flicker.

| Section | Main Controls |
| --- | --- |
| **Quick** | Output view (Normal / Original / Split / Difference), Output mix, Overall strength, Model preset, Pass 1 basics |
| **NR passes** | NR passes (1–3) + single-pass editor (Style, Intensity, Local tone/structure, Skin structure, UI correction) |
| **Color & protection** | Chroma, Lightness, Midtone, Darkening/Brightening, Hue/Dark/Highlight/Chroma protection, Low/High frequency |
| **Performance & anti-flicker** | Manual/Adaptive scaling & response timing, Alternating frame cadence & reuse mode, AMDOF/NVOF flow & anti-flicker |
| **Runtime & diagnostics** | Custom runtime DLL path, GPU & timing stats, Correction diagnostics, Refresh stats, Copy diagnostics, Retry init |

<details>
<summary>Expand: Advanced behavior notes (cadence & async resource swaps)</summary>

- **Cadence tracks OBS video ticks**: A 30 FPS source on a 60 FPS canvas steps on the 60 FPS canvas cadence; extra preview windows reuse the same cycle without advancing the interval.
- **Background resource preparation**: Changing model presets, pass count, inference scale, or optical-flow mode prepares and retires GPU resources asynchronously while retaining the latest completed frame, then switches once ready.

</details>

---

## Troubleshooting & Support

- **Filter missing in OBS?**
  - Verify `obs-plugins\64bit\obs-dlss5.dll` and `data\obs-plugins\obs-dlss5\` sit directly under the **OBS root folder** (no extra nested ZIP folder).
  - Ensure OBS uses the **Direct3D 11** renderer (`Settings → Advanced → Video → Renderer`).
- **Reporting issues**: Open the **Runtime & diagnostics** section, click **Copy diagnostics** (or **Retry initialization** if needed), and submit an [issue](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/issues) with the copied diagnostics, OBS version, GPU, driver version, and steps to reproduce.

---

## Building & License

- **Build guide**: Dependencies and CMake commands are documented in [BUILDING.md](docs/BUILDING.md).
- **Package guide**: Also bundled inside the ZIP as `安装说明.txt` ([DISTRIBUTION-INSTALL.md](docs/DISTRIBUTION-INSTALL.md)).
- **License**: Plugin source is licensed under **GNU GPL v3 (GPL-3.0-only)** ([LICENSE](LICENSE)). Third-party component licenses and bundled runtime notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
