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

The contract suite covers parameters, host capability binding, GPU identity and alpha, fence completion, real-time output, optical flow and temporal processing. The optional `obs_harness` loads the production plugin into an OBS runtime for image and preview-path checks. Image fixtures use the Python dependencies listed in `tests/requirements.txt`.

## Package

```powershell
python .\scripts\Prepare-Distribution.py --version 0.1.0 --runtime 'D:\deps\nvngx_dlssnr.dll'
python .\scripts\Package-Verified.py --candidate dist/0.1.0 --output-stem DLSS5-Neural-Filter-0.1.0
```

The first command creates a fresh versioned directory with the plugin, runtime, resources, user documentation and installation tools. It will not merge into an existing versioned directory.

The second command checks the DLL and resources, pairs the package with the current source, and creates the Windows ZIP, source ZIP and SHA256 files. Source selection includes production code, resources, licenses, build/install scripts, tests and the public build/installation guides. Local test output, SDKs, historical notes and machine-specific helper scripts are excluded.

To check installation and rollback in temporary directories:

```powershell
.\tests\DeliveryContracts.ps1 -PackageRoot .\dist\0.1.0
.\tests\DistributionContracts.ps1 -PackageRoot .\dist\0.1.0
```

The OBS build-header version is independent of runtime admission; the plugin checks the required public APIs and graphics capabilities.

## 中文说明

在仓库根目录运行命令；源码 ZIP 解压后的 `obs` 目录就是项目根目录。准备 VS 2022 C++、Windows SDK、CMake、Python，以及上面列出的 OBS、DLSS/NGX、FidelityFX 和 Optical Flow 依赖，再将示例路径替换为实际位置。

`Build-Local.ps1` 构建 Release 并运行合同测试；提供 `-RuntimeDll` 时执行真实神经推理检查。`Prepare-Distribution.py` 整理安装目录，`Package-Verified.py` 生成配套二进制、源码 ZIP 和校验文件。安装与回退脚本的隔离检查见上面的 PowerShell 命令。

项目源码采用 GPL-3.0-only；外部依赖许可见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
