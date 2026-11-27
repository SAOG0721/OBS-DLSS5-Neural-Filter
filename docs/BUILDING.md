# Building DLSS5 Neural Filter

Run commands from the repository root. In the separately provided source ZIP, the project root is the `obs` folder.

## Dependencies

- Windows x64, Visual Studio 2022 C++ and a Windows SDK.
- CMake 3.25 or later, Python 3.11 or later, and Windows PowerShell.
- OBS Studio public headers. The current build uses the OBS 32.2.2 headers.
- NVIDIA DLSS/NGX SDK containing `include/nvsdk_ngx.h` and `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`.
- AMD FidelityFX SDK v2.3.0, including `Kits/FidelityFX`.
- NVIDIA Optical Flow API headers, including `nvOpticalFlowD3D11.h` and `nvOpticalFlowD3D12.h`. The current build uses the headers from dxvk-nvapi's `inc/nvofapi`.
- The x64 Windows SDK `dxc.exe` shader compiler.
- A compatible `nvngx_dlssnr.dll` for neural GPU tests and distribution packaging.

SDKs are supplied separately. The build reads their paths from explicit parameters; the installed plugin uses its private runtime folder.

## Build and test

Use a Visual Studio developer PowerShell and replace the example dependency paths:

```powershell
.\scripts\Build-Local.ps1 `
  -ObsSourceRoot 'D:\deps\obs-studio-32.2.2' `
  -NgxSdkRoot 'D:\deps\DLSS' `
  -FidelityFxSdkRoot 'D:\deps\FSR-SDK-v2.3.0' `
  -NvOfSdkRoot 'D:\deps\nvofapi' `
  -DxcPath 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe' `
  -RuntimeDll 'D:\deps\nvngx_dlssnr.dll'
```

The script configures a Release build, runs CTest, and runs neural GPU checks when a runtime is supplied.

- Production DLL: `build/Release/obs-dlss5.dll`
- Staged resources: `build/package/obs-dlss5`
- Test results: `tests/results`

The contract suite covers parameters, host capability binding, GPU identity and alpha, fence completion, real-time output, optical flow, color/residual reconstruction and temporal enhancement reuse. The optional `obs_harness` loads the production plugin into an OBS runtime for image, media and preview-path checks. Image fixtures use the Python dependencies listed in `tests/requirements.txt`.

Generate procedural image, motion and parameter cases for the host harness:

```powershell
python .\tests\PrepareSync0612Fixtures.py prepare --output .\tests\results\sync-fixtures
.\build\Release\obs_harness.exe 'D:\OBS' .\build\package\obs-dlss5\bin\64bit\obs-dlss5.dll .\tests\results\sync-fixtures\alpha-1280x720.png --private-runtime .\tests\results\host-ht --check .\build\package\obs-dlss5\data .\tests\results\sync-fixtures\settings\ht-manual.json
```

The runtime root supplies OBS's libraries and modules. The harness does not open the user's OBS configuration. The last argument is an optional settings JSON; the runtime argument always overrides any runtime path in that JSON. `--dual-preview` checks both preview color spaces, `--media` accepts a generated MP4, and `--media-record` also creates short NVENC original/enhanced recordings. Inspect them with `python tests/PrepareSync0612Fixtures.py verify-recordings <output-directory>`.

For same-process runtime changes, set `NEURAL_HARNESS_ALTERNATE_RUNTIME` to a different compatible runtime DLL's absolute path and use `--runtime-switch`. The harness switches to that DLL and back, requiring a stable idle/processing window with non-original output and unchanged alpha after each switch.

For NVENC recording, run a copy of the harness located in the isolated OBS runtime's `bin/64bit` directory and pass absolute input/output paths. OBS locates `obs-nvenc-test.exe` and its mux helper relative to the executable, so those helpers must be beside the harness for this test.

## Package

```powershell
python .\scripts\Prepare-Distribution.py --version 0.2.1 --runtime 'D:\deps\community\nvngx_dlssnr.dll' --rtx50-runtime 'D:\deps\rtx50\nvngx_dlssnr.dll'
python .\scripts\Package-Verified.py --candidate dist/0.2.1 --output-stem DLSS5-Neural-Filter-0.2.1
```

The first command creates a fresh internal staging directory with the plugin, runtime, resources, licenses, a short installation guide and a validation manifest. It will not merge into an existing versioned directory.

`--rtx50-runtime` is optional and includes the NVIDIA original as a selectable option in the same main ZIP. A release build can supply `--validation-report report.json`; the report's `PluginSHA256` and `RuntimeSHA256` must match the selected DLLs. It provides `Validation`, `ValidatedHostVersions`, `ValidatedGPU` and `PendingAcceptance`. Without a paired report, the manifest records no validation claims.

The second command checks the DLL and resources, pairs the package with the current source, and creates the Windows ZIP, source ZIP and SHA256 files. The Windows ZIP contains only `obs-plugins/`, `data/` and `安装说明.txt`: its directories can be merged directly into the OBS installation root. Licenses and third-party notices are in `data/obs-plugins/obs-dlss5/`. Internal manifests and installation scripts are excluded from that ZIP.

Source selection includes production code, resources, licenses, developer scripts, tests and the public build/installation guides. Local test output, SDKs, historical notes and machine-specific helper scripts are excluded. The staging manifest records both its internal file paths and the final archive paths; packaging checks the exact archived file set, hashes and ZIP CRC.

The source archive records hashes for the actual packaged files. Clean source is identified by its commit; modified or added source is identified as a working-tree snapshot with a base commit. Building and packaging do not require committing local work.

The OBS build-header version is independent of runtime admission; the plugin checks the required public APIs and graphics capabilities.

## Performance probes

The native resource-preparation contract requires a compatible runtime:

```powershell
.\build\Release\backend_performance_contracts.exe --prepare-native 'D:\deps\nvngx_dlssnr.dll'
.\build\Release\host_performance_probe.exe 'D:\OBS' .\build\package\obs-dlss5\bin\64bit\obs-dlss5.dll 'D:\fixtures\image.png' 'D:\deps\nvngx_dlssnr.dll' .\tests\results\host-performance .\build\package\obs-dlss5\data 120 5
```

The host probe uses an isolated libobs context without the user's OBS configuration. It separates settings submission, CPU render calls, graphics-lock time and property refresh requests. Timed render loops contain no GPU readback. It waits for background work before steady-state samples and checks that the final manual inference size becomes active after a sweep. Available retained output is reported separately from parameter acceptance; CPU render latency is not the full effect-switching latency. Native Qt input, layout and paint time need a window-level check.

`gpu_contracts --benchmark <width> <height> <runtime> <fps> <frames> <percent> <preset>` reports D3D12 stage timings and CPU submission costs. This offline timing mode uses a longer fence budget to tolerate concurrent GPU work; the production real-time budget remains unchanged. Keep the background workload consistent and repeat before/after runs before claiming throughput gains.

## 中文说明

在仓库根目录运行命令；源码 ZIP 解压后的 `obs` 目录就是项目根目录。准备 VS 2022 C++、Windows SDK、CMake、Python，以及上面列出的 OBS、DLSS/NGX、FidelityFX 和 Optical Flow 依赖，再将示例路径替换为实际位置。

`Build-Local.ps1` 构建 Release 并运行合同测试；提供 `-RuntimeDll` 时执行真实神经推理检查。`Prepare-Distribution.py` 整理内部暂存目录，`Package-Verified.py` 生成配套二进制、源码 ZIP 和校验文件。主分发包仅保留与 OBS 根目录对应的 `obs-plugins`、`data` 及 `安装说明.txt`，复制合并即可安装；打包时检查实际归档的文件集合、哈希和 ZIP CRC。

项目源码采用 GPL-3.0-only；外部依赖许可见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
