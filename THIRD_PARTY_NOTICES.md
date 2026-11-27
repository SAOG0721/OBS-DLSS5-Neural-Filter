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

The DLSSNR DLLs are separate NVIDIA runtime components. Their licensing is separate from the GPL-3.0-only license for the plugin source. Both options retain the filenames supplied with Magpie Experimental 0.6.12. The [NVIDIA RTX SDK license](licenses/NVIDIA-RTX-SDK.txt) is included with the package.

| File | Version | Build | SHA256 |
| --- | --- | --- | --- |
| `runtime/nvngx_dlssnr.dll` | `310.9.16.0` | Community compatibility build, default | `A8A237A967469C455A84654932B513AB656FBAA1E0C44504B1E1BF0890F902B8` |
| `runtime/rtx50/nvngx_dlssnr.dll` | `310.9.16.0` | NVIDIA original RTX 50 option | `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F` |

The compatibility build has community modifications. The NVIDIA original retains its original signature.

Checksums identify the supplied files for integrity checking. The plugin also accepts other compatible runtime builds.
