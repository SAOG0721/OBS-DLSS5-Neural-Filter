# Third-party notices

DLSS5 Neural Filter source code is licensed under **GPL-3.0-only**. The full license is in [LICENSE](LICENSE).

## Code and SDK components

- **Resolve integration helpers — Jay, MIT.** Feature 18 helpers, parameter definitions and NGX integration code include material from the author's separately MIT-licensed Resolve integration. See [Resolve-Integration-MIT.txt](licenses/Resolve-Integration-MIT.txt); the retained parameter header is `src/vendor/Feature18Parameters.h`.
- **AMD FidelityFX 2.3.0 — MIT.** Optical Flow shaders and the incorporated D3D12 backend use AMD FidelityFX code. See [AMD-FidelityFX-MIT.txt](licenses/AMD-FidelityFX-MIT.txt). The project's shader-generation script is GPL-3.0-only.
- **NVIDIA Optical Flow headers — MIT.** The API headers used through dxvk-nvapi retain NVIDIA's MIT notice. See [NVIDIA-OpticalFlow-MIT.txt](licenses/NVIDIA-OpticalFlow-MIT.txt). The driver component `nvofapi64.dll` is loaded from Windows System32.
- **NVIDIA DLSS/NGX SDK.** Headers and the static loader are covered by the [NVIDIA RTX SDK license](licenses/NVIDIA-RTX-SDK.txt). This software contains source code provided by NVIDIA Corporation.
- **OBS Studio — GPL-2.0-or-later.** The plugin uses OBS public headers and dynamically resolves the host API. OBS itself is installed separately.
- **Oklab matrices.** The color transforms use Björn Ottosson's public-domain formulation.

## Bundled runtime

`nvngx_dlssnr.dll` is a separate NVIDIA runtime component with community modifications. Its licensing is separate from the GPL-3.0-only license for the plugin source.

- File version: `310.8.0.0`
- SHA256: `984BEE0F775C277D5829B8FD6775D53A7B0F75396C852B3AAF06A18375F81014`
- This modified component reports Authenticode status `0x80096010` (bad digest), rather than an intact NVIDIA signature.

Checksums identify the supplied files for integrity checking. The plugin also accepts other compatible runtime builds.
