# DLSS5 Neural Filter for OBS

[简体中文](README.md) · [Downloads](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases) · [Installation guide](docs/DISTRIBUTION-INSTALL.md#english)

A Windows x64 **OBS source effect filter** that brings DLSSNR neural image enhancement to game capture, video, camera and other sources for streaming and recording.

This is a spin-off plugin by the author of [Magpie Experimental (大力喜鹊)](https://github.com/SAOG0721/Magpie), bringing its neural image-enhancement controls into the OBS source-filter workflow.

## Features

| Feature | Controls |
| --- | --- |
| Multiple NR passes | 1–3 passes, each with its own style, NR intensity, local tone, local structure, skin structure, auto mask and UI correction |
| Effect and color | Output mix, overall strength, chroma, lightness, darkening and brightening contributions |
| Protection and detail | Hue, dark-area and highlight protection, excess correction suppression, low- and high-frequency strength |
| Motion and anti-flicker | AMDOF / NVOF optical flow; Static, Flow, Flow+ and Low Frequency anti-flicker modes, with an Off option |
| Comparison views | Normal, Original, Split and Difference |
| Inference scaling | 25%–100% inference resolution while preserving the source output size |
| Real-time output | Previews share completed results; the latest completed image is retained while the GPU is busy |
| Ready to install | Bundled runtime with automatic discovery, Chinese/English controls, installation and rollback tools |

## Requirements

- Windows x64 and OBS Studio using the Direct3D 11 renderer.
- An NVIDIA RTX GPU and driver compatible with the bundled DLSSNR runtime.
- SDR processing; HDR sources and HDR canvases pass through.
- Source sizes up to 3840 × 2160; inference sizes from 256 × 144 to 3840 × 2160.

The verified configuration is OBS 32.2.2, RTX 5070 Ti and driver 616.92. Compatibility is determined by required interfaces and graphics capabilities, with no OBS version-number gate.

## Installation

Download **Windows-x64.zip** from [Releases](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/releases), extract the complete archive and close OBS. For an update, use the same installation layout as before.

### Standard OBS

Right-click **Install.cmd → Run as administrator**.

The default destination is `C:\ProgramData\obs-studio\plugins\obs-dlss5`.

### Steam / portable OBS

Drag the **OBS installation root folder** onto **Install-OBS.cmd**. The folder should contain `bin\64bit\obs64.exe`.

Alternatively, open PowerShell in the extracted folder and provide your OBS path:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-OBS.ps1 -ObsRoot 'D:\SteamLibrary\steamapps\common\OBS Studio' -Apply
```

See the [installation guide](docs/DISTRIBUTION-INSTALL.md#english) for manual installation and rollback.

## Getting started

1. Select a source in OBS and open **Filters → Effect Filters → DLSS5 Neural Filter**.
2. Leave **DLSSNR runtime DLL** empty to use the bundled runtime.
3. Begin with one NR pass, 100% inference resolution, optical flow off and anti-flicker off.
4. Select **Split** to compare the result, then adjust output mix, overall strength and per-pass controls.
5. Enable flow and anti-flicker as needed. Reduce the pass count or inference resolution to lower GPU load.

**Show advanced controls** exposes protection and detail controls. Source transparency is preserved, and filter settings are saved with your OBS scene collection.

## Updates, rollback and support

Installation backs up replaced files and prints a rollback receipt path. Backups are stored in `%LOCALAPPDATA%\Dlss5NeuralFilter\Backups`. Follow the [installation guide](docs/DISTRIBUTION-INSTALL.md#english) to restore an earlier installation.

Use **Retry initialization** or **Copy diagnostics** in the filter when needed. Include diagnostics, your OBS version, GPU and reproduction steps when opening an [issue](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter/issues).

## Source and license

Clone this repository or use GitHub's source downloads to obtain the source code. Build requirements and commands are in [BUILDING.md](docs/BUILDING.md).

The plugin source is licensed under **GNU GPL v3 (GPL-3.0-only)**; see [LICENSE](LICENSE). Component licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
