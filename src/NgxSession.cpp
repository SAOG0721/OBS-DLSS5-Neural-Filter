// SPDX-License-Identifier: MIT
// SEH and private-runtime compatibility adapted from Jay's MIT Resolve integration.
#include "NgxSession.h"
#include "vendor/Feature18Parameters.h"
#include <Windows.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace neural {
namespace {
using namespace resolve_dlss5;
using Microsoft::WRL::ComPtr;

constexpr NVSDK_NGX_Feature kFeatureDlssNr = static_cast<NVSDK_NGX_Feature>(18);
constexpr unsigned long long kSignedSnippetApplicationId = 0x0876232Cull;
constexpr char kProjectId[] = "7c134ab9-9677-4af5-a2b2-bca943350861";
constexpr char kEngineVersion[] = "OBS-DLSS5-Neural-Filter";

using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                                       ID3D12Device*, NVSDK_NGX_Version,
                                                       const NVSDK_NGX_Parameter*);
using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                      NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                                        const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*,
                                                        PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

std::atomic<HMODULE> g_callerModule = nullptr;
std::atomic<GetModuleFileNameWFn> g_originalGetModuleFileNameW = nullptr;
std::mutex g_logMutex;
std::mutex g_hookMutex;
std::mutex g_runtimeExecutionMutex;
struct CallerHook { HMODULE module=nullptr; std::uint32_t references=0; };
std::map<void**,CallerHook> g_callerHooks;

template <typename T>
void* functionAddress(T function) noexcept {
    void* result = nullptr;
    static_assert(sizeof(function) == sizeof(result));
    std::memcpy(&result, &function, sizeof(result));
    return result;
}

template <typename T>
T getExport(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool ngxSucceeded(NVSDK_NGX_Result result) noexcept { return NVSDK_NGX_SUCCEED(result); }

LONG captureNgxException(DWORD code, DWORD* sehCode) noexcept {
    *sehCode = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

NVSDK_NGX_Result callCoreInitSafely(const wchar_t* applicationDataPath, ID3D12Device* device,
                                    const NVSDK_NGX_FeatureCommonInfo* featureInfo,
                                    DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                   kEngineVersion, applicationDataPath, device,
                                                   featureInfo, NVSDK_NGX_Version_API);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callAllocateParametersSafely(NVSDK_NGX_Parameter** parameters,
                                              DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_AllocateParameters(parameters);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callCreateFeatureSafely(CreateFeatureFn function,
                                         ID3D12GraphicsCommandList* commandList,
                                         NVSDK_NGX_Parameter* parameters,
                                         NVSDK_NGX_Handle** feature, DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return function(commandList, kFeatureDlssNr, parameters, feature);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callEvaluateFeatureSafely(EvaluateFeatureFn function,
                                           ID3D12GraphicsCommandList* commandList,
                                           const NVSDK_NGX_Handle* feature,
                                           const NVSDK_NGX_Parameter* parameters,
                                           DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return function(commandList, feature, parameters, nullptr);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callReleaseFeatureSafely(ReleaseFeatureFn function, NVSDK_NGX_Handle* feature,
                                          DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return function(feature);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callDestroyParametersSafely(NVSDK_NGX_Parameter* parameters,
                                             DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_DestroyParameters(parameters);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callShutdownSafely(ShutdownFn function, ID3D12Device* device,
                                    DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return function(device);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result callSnippetInitSafely(SnippetInitExtFn function,
                                       const wchar_t* applicationDataPath, ID3D12Device* device,
                                       DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        return function(kSignedSnippetApplicationId, applicationDataPath, device,
                        NVSDK_NGX_Version_API, nullptr);
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result NVSDK_CONV setScalingRatioCallback(NVSDK_NGX_Parameter* parameters) noexcept {
    __try {
        if (!parameters) {
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        }
        parameters->Set(ngx_parameter::ScalingRatio, 1.0F);
        return NVSDK_NGX_Result_Success;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

bool setCreateParametersSafely(NVSDK_NGX_Parameter* parameters, unsigned int width,
                               unsigned int height, int preset, DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        parameters->Set(ngx_parameter::Width, width);
        parameters->Set(ngx_parameter::Height, height);
        parameters->Set(ngx_parameter::InputWidth, width);
        parameters->Set(ngx_parameter::InputHeight, height);
        parameters->Set(ngx_parameter::OutputWidth, width);
        parameters->Set(ngx_parameter::OutputHeight, height);
        parameters->Set(ngx_parameter::OutputDotWidth, width);
        parameters->Set(ngx_parameter::OutputDotHeight, height);
        parameters->Set(ngx_parameter::Upscaling, 0U);
        parameters->Set(ngx_parameter::Scale, 1.0F);
        parameters->Set(ngx_parameter::ScalingRatio, 1.0F);
        parameters->Set(ngx_parameter::ScalingRatioCallback,
                        functionAddress(&setScalingRatioCallback));
        parameters->Set(ngx_parameter::Preset, preset);
        parameters->Set(NVSDK_NGX_Parameter_Width, width);
        parameters->Set(NVSDK_NGX_Parameter_Height, height);
        parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                        static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
        parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1U);
        parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1U);
        return true;
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return false;
    }
}

void setFullSubrect(NVSDK_NGX_Parameter* parameters, const char* baseX, const char* baseY,
                    const char* widthName, const char* heightName, unsigned int width,
                    unsigned int height) {
    parameters->Set(baseX, 0U);
    parameters->Set(baseY, 0U);
    parameters->Set(widthName, width);
    parameters->Set(heightName, height);
}

bool setEvaluateParametersSafely(NVSDK_NGX_Parameter* parameters, ID3D12Resource* color,
                                 ID3D12Resource* output, ID3D12Resource* motion,
                                 ID3D12Resource* depth, unsigned int width, unsigned int height,
                                 const Feature18Settings* settings, bool resetHistory,
                                 DWORD* sehCode) noexcept {
    *sehCode = 0;
    __try {
        parameters->Set(ngx_parameter::Color, color);
        parameters->Set(ngx_parameter::Output, output);
        parameters->Set(ngx_parameter::MotionVectors, motion);
        parameters->Set(ngx_parameter::Depth, depth);
        setFullSubrect(parameters, ngx_parameter::ColorSubrectBaseX,
                       ngx_parameter::ColorSubrectBaseY, ngx_parameter::ColorSubrectWidth,
                       ngx_parameter::ColorSubrectHeight, width, height);
        setFullSubrect(parameters, ngx_parameter::OutputSubrectBaseX,
                       ngx_parameter::OutputSubrectBaseY, ngx_parameter::OutputSubrectWidth,
                       ngx_parameter::OutputSubrectHeight, width, height);
        setFullSubrect(parameters, ngx_parameter::MotionSubrectBaseX,
                       ngx_parameter::MotionSubrectBaseY, ngx_parameter::MotionSubrectWidth,
                       ngx_parameter::MotionSubrectHeight, width, height);
        setFullSubrect(parameters, ngx_parameter::DepthSubrectBaseX,
                       ngx_parameter::DepthSubrectBaseY, ngx_parameter::DepthSubrectWidth,
                       ngx_parameter::DepthSubrectHeight, width, height);
        parameters->Set(ngx_parameter::MotionScaleX, settings->motionScaleX);
        parameters->Set(ngx_parameter::MotionScaleY, settings->motionScaleY);
        const int depthInverted = settings->depthConvention == DepthConvention::ForceNormal ? 0 : 1;
        parameters->Set(ngx_parameter::DepthInverted, depthInverted);
        parameters->Set(ngx_parameter::IndicatorInvertX, 0);
        parameters->Set(ngx_parameter::IndicatorInvertY, 0);
        parameters->Set(ngx_parameter::Enabled, 1);
        parameters->Set(ngx_parameter::Reset, resetHistory ? 1 : 0);
        parameters->Set(ngx_parameter::Style, settings->style);
        parameters->Set(ngx_parameter::Intensity, settings->intensity);
        parameters->Set(ngx_parameter::LocalToneStrength, settings->localToneStrength);
        parameters->Set(ngx_parameter::LocalStructureStrength, settings->localStructureStrength);
        parameters->Set(ngx_parameter::SkinStructureStrength, settings->skinStructureStrength);
        parameters->Set(ngx_parameter::UseAutoMask, settings->useAutoMask ? 1 : 0);
        parameters->Set(ngx_parameter::UiCorrection, settings->uiCorrection ? 1 : 0);
        return true;
    } __except (captureNgxException(GetExceptionCode(), sehCode)) {
        return false;
    }
}

DWORD WINAPI snippetGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) noexcept {
    if (module == g_callerModule.load(std::memory_order_acquire)) {
        constexpr wchar_t kAuthorizedCaller[] = L"nvngx.dll";
        constexpr DWORD kAuthorizedLength = ARRAYSIZE(kAuthorizedCaller) - 1;
        if (!filename || !size) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (size <= kAuthorizedLength) {
            if (size > 1) {
                std::memcpy(filename, kAuthorizedCaller,
                            static_cast<std::size_t>(size - 1) * sizeof(wchar_t));
            }
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, kAuthorizedCaller, sizeof(kAuthorizedCaller));
        return kAuthorizedLength;
    }

    const auto original = g_originalGetModuleFileNameW.load(std::memory_order_acquire);
    if (original) {
        return original(module, filename, size);
    }
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

void** findImportedFunctionSlot(HMODULE module, const char* functionName) noexcept {
    if (!module || !functionName) {
        return nullptr;
    }
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size ||
        directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
        directory.Size > nt->OptionalHeader.SizeOfImage ||
        directory.VirtualAddress > nt->OptionalHeader.SizeOfImage - directory.Size) {
        return nullptr;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress + directory.Size);
    for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
        if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) {
            continue;
        }
        const char* libraryName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
            _stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
            continue;
        }
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) {
            continue;
        }
        auto* nameThunk =
            reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) {
                continue;
            }
            const auto nameRva = static_cast<std::uint32_t>(nameThunk->u1.AddressOfData);
            if (nameRva >= nt->OptionalHeader.SizeOfImage) {
                return nullptr;
            }
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameRva);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) == 0) {
                return reinterpret_cast<void**>(&addressThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool installSnippetCallerCompatibility(HMODULE snippetModule, void*** slotOut) noexcept {
    std::scoped_lock lock(g_hookMutex);
    void** slot = findImportedFunctionSlot(snippetModule, "GetModuleFileNameW");
    if (!slot) {
        return false;
    }
    if (auto existing=g_callerHooks.find(slot);existing!=g_callerHooks.end()) {
        if (existing->second.module!=snippetModule) return false;
        ++existing->second.references;
        *slotOut = slot;
        writeDiagnosticLog("Reused DLSSNR caller hook reference");
        return true;
    }
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    const auto systemFunction=kernel ? GetProcAddress(kernel,"GetModuleFileNameW") : nullptr;
    if (!systemFunction || *slot != functionAddress(systemFunction)) {
        writeDiagnosticLog("Private runtime caller slot is already owned by another hook");
        return false;
    }
    const void* hookAddress = functionAddress(&snippetGetModuleFileNameW);
    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(hookAddress), &callerModule)) {
        return false;
    }
    DWORD oldProtection = 0;
    try { g_callerHooks.emplace(slot,CallerHook{snippetModule,1}); }
    catch (...) { return false; }
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
        g_callerHooks.erase(slot);
        return false;
    }
    g_callerModule.store(callerModule, std::memory_order_release);
    GetModuleFileNameWFn originalFunction = reinterpret_cast<GetModuleFileNameWFn>(systemFunction);
    g_originalGetModuleFileNameW.store(originalFunction, std::memory_order_release);
    void* original = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                                                const_cast<void*>(hookAddress),functionAddress(systemFunction));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    if (original!=functionAddress(systemFunction)) { g_callerHooks.erase(slot); return false; }
    *slotOut = slot;
    writeDiagnosticLog("Installed process-level DLSSNR caller hook: refs=1");
    return true;
}

bool restoreSnippetCallerCompatibility(void** slot) noexcept {
    if (!slot) {
        return true;
    }
    std::scoped_lock lock(g_hookMutex);
    auto existing=g_callerHooks.find(slot);
    if (existing==g_callerHooks.end()) return false;
    if (existing->second.references > 1) {
        --existing->second.references;
        writeDiagnosticLog("Released shared DLSSNR caller hook reference");
        return true;
    }
    const auto original = g_originalGetModuleFileNameW.load(std::memory_order_acquire);
    DWORD oldProtection = 0;
    if (!original || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    const auto replaced=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
        functionAddress(original),functionAddress(&snippetGetModuleFileNameW));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    if (replaced!=functionAddress(&snippetGetModuleFileNameW)) return false;
    g_callerHooks.erase(existing);
    writeDiagnosticLog("Restored process-level DLSSNR caller hook: refs=0");
    return true;
}

std::filesystem::path moduleFile() {
    HMODULE module = nullptr;
    const void* address = functionAddress(&moduleFile);
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)) {
        return {};
    }
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path);
}
std::filesystem::path moduleDirectory() { return moduleFile().parent_path(); }

std::filesystem::path applicationDataDirectory() {
    std::wstring root(32768, L'\0');
    const DWORD size =
        GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), static_cast<DWORD>(root.size()));
    if (!size || size >= root.size()) {
        return moduleDirectory();
    }
    root.resize(size);
    return std::filesystem::path(root) / L"Dlss5NeuralFilter" / L"NGX";
}


std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n && !s.empty()) throw std::runtime_error("Runtime path is not valid UTF-8");
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string sha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Open SHA256 provider failed");
    const auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) { BCryptCloseAlgorithmProvider(algorithm,0); throw std::runtime_error("Create SHA256 failed"); }
    std::ifstream f(path, std::ios::binary);
    char chunk[65536]; bool okay = static_cast<bool>(f);
    while (f && f.read(chunk,sizeof(chunk)).gcount()) {
        if (BCryptHashData(hash,reinterpret_cast<PUCHAR>(chunk),static_cast<ULONG>(f.gcount()),0) < 0) okay=false;
    }
    if (f.bad()) okay=false;
    unsigned char digest[32]{};
    if (BCryptFinishHash(hash,digest,sizeof(digest),0) < 0) okay=false;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0);
    if (!okay) throw std::runtime_error("Runtime SHA256 read failed");
    std::ostringstream s; s << std::hex << std::setfill('0');
    for (auto b : digest) s << std::setw(2) << static_cast<unsigned>(b);
    return s.str();
}
std::string runtimeIdentity(const std::filesystem::path& path, const DXGI_ADAPTER_DESC1& gpu, IDXGIAdapter1* adapter) {
    std::ostringstream s;
    s << "runtime=" << utf8(path.wstring()) << "; sha256=" << sha256(path);
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<unsigned char> version(size);
    VS_FIXEDFILEINFO* info = nullptr; UINT infoSize = 0;
    if (size && GetFileVersionInfoW(path.c_str(),0,size,version.data()) &&
        VerQueryValueW(version.data(),L"\\",reinterpret_cast<void**>(&info),&infoSize) && infoSize >= sizeof(*info))
        s << "; version=" << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS)
          << '.' << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    WINTRUST_FILE_INFO file{}; file.cbStruct=sizeof(file); file.pcwszFilePath=path.c_str();
    WINTRUST_DATA trust{}; trust.cbStruct=sizeof(trust); trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE; trust.dwUnionChoice=WTD_CHOICE_FILE; trust.pFile=&file;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL; trust.dwStateAction=WTD_STATEACTION_VERIFY;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result=WinVerifyTrust(nullptr,&policy,&trust);
    trust.dwStateAction=WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr,&policy,&trust);
    s << "; Authenticode=" << std::hex << static_cast<unsigned long>(result) << std::dec
      << "; gpu=" << utf8(gpu.Description) << "; pci=" << std::hex << gpu.VendorId << ':' << gpu.DeviceId
      << "; luid=" << static_cast<unsigned>(gpu.AdapterLuid.HighPart) << ':' << gpu.AdapterLuid.LowPart << std::dec;
    s << "; path=experimental-direct-snippet; motion=current-to-previous/pixels/y-down; depth=constant";
    LARGE_INTEGER driver{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))
        s << "; driver=" << HIWORD(driver.HighPart) << '.' << LOWORD(driver.HighPart)
          << '.' << HIWORD(driver.LowPart) << '.' << LOWORD(driver.LowPart);
    else s << "; driver=unreported";
    using OsVersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto versionFn=reinterpret_cast<OsVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
    OSVERSIONINFOW os{}; os.dwOSVersionInfoSize=sizeof(os);
    if (versionFn && versionFn(&os)==0)
        s << "; Windows=" << os.dwMajorVersion << '.' << os.dwMinorVersion << '.' << os.dwBuildNumber;
    const auto plugin=moduleFile();
    if (!plugin.empty()) s << "; pluginBinary=" << utf8(plugin.wstring()) << "; pluginSHA256=" << sha256(plugin);
    return s.str();
}
void checkNgx(const char* phase, NVSDK_NGX_Result result, DWORD seh) {
    if (!seh && ngxSucceeded(result)) return;
    std::ostringstream s; s << phase << " result=0x" << std::hex << static_cast<unsigned>(result)
                            << " SEH=0x" << seh;
    throw std::runtime_error(s.str());
}
std::mutex registryMutex;
std::map<std::wstring,std::weak_ptr<NgxSession>> sessions;
} // namespace
void writeDiagnosticLog(const std::string& message) noexcept {
    try { const auto s="[DLSS5 Neural Filter] "+message+"\n"; OutputDebugStringA(s.c_str()); } catch (...) {}
}
struct NgxSession::Impl {
    HMODULE module=nullptr; void** hook=nullptr;
    CreateFeatureFn create=nullptr; EvaluateFeatureFn evaluate=nullptr;
    ReleaseFeatureFn release=nullptr; ShutdownFn shutdown=nullptr;
    bool core=false, snippet=false;
};
NgxSession::NgxSession() : impl(std::make_unique<Impl>()) {}
std::string resolveRuntimePath(const std::string& selection) {
    std::filesystem::path path;
    if (!selection.empty()) path=std::filesystem::path(wide(selection));
    else {
        path=moduleDirectory()/L"runtime"/L"nvngx_dlssnr.dll";
        std::error_code error;
        if (!std::filesystem::is_regular_file(path,error)) {
            const auto adjacent=moduleDirectory()/L"nvngx_dlssnr.dll";
            if (std::filesystem::is_regular_file(adjacent,error)) path=adjacent;
        }
    }
    return utf8(std::filesystem::weakly_canonical(path).wstring());
}
std::shared_ptr<NgxSession> NgxSession::acquire(IDXGIAdapter1* adapter,const std::string& selection) {
    const auto path=std::filesystem::path(wide(resolveRuntimePath(selection)));
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Runtime DLL not found: "+utf8(path.wstring()));
    DXGI_ADAPTER_DESC1 gpu{}; if (FAILED(adapter->GetDesc1(&gpu))) throw std::runtime_error("Read OBS adapter identity failed");
    const auto key=path.wstring()+L"|"+std::to_wstring(gpu.AdapterLuid.HighPart)+L":"+std::to_wstring(gpu.AdapterLuid.LowPart);
    std::scoped_lock lock(registryMutex);
    if (auto cached=sessions[key].lock(); cached && SUCCEEDED(cached->device->GetDeviceRemovedReason())) return cached;
    auto session=std::shared_ptr<NgxSession>(new NgxSession);
    if (GetEnvironmentVariableW(L"NEURAL_DRED",nullptr,0)) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }
    if (GetEnvironmentVariableW(L"NEURAL_D3D12_DEBUG",nullptr,0)) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
    const auto created=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&session->device));
    if (FAILED(created)) {
        std::ostringstream message; message << "Create D3D12 device failed, HRESULT=0x" << std::hex << static_cast<unsigned>(created);
        throw std::runtime_error(message.str());
    }
    session->identity=runtimeIdentity(path,gpu,adapter);
    const auto data=applicationDataDirectory(); std::filesystem::create_directories(data);
    const auto directory=path.parent_path().wstring(); const wchar_t* paths[]{directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo featureInfo{}; featureInfo.PathListInfo.Path=paths; featureInfo.PathListInfo.Length=1;
    DWORD seh=0; auto result=callCoreInitSafely(data.c_str(),session->device.Get(),&featureInfo,&seh);
    checkNgx("NGX core init",result,seh); session->impl->core=true;
    session->impl->module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!session->impl->module) throw std::runtime_error("Load selected runtime failed, Win32="+std::to_string(GetLastError()));
    auto& impl=*session->impl;
    const auto init=getExport<SnippetInitExtFn>(impl.module,"NVSDK_NGX_D3D12_Init_Ext");
    impl.create=getExport<CreateFeatureFn>(impl.module,"NVSDK_NGX_D3D12_CreateFeature");
    impl.evaluate=getExport<EvaluateFeatureFn>(impl.module,"NVSDK_NGX_D3D12_EvaluateFeature");
    impl.release=getExport<ReleaseFeatureFn>(impl.module,"NVSDK_NGX_D3D12_ReleaseFeature");
    impl.shutdown=getExport<ShutdownFn>(impl.module,"NVSDK_NGX_D3D12_Shutdown1");
    if (!init || !impl.create || !impl.evaluate || !impl.release || !impl.shutdown)
        throw std::runtime_error("Selected runtime lacks required Feature 18 exports");
    if (!installSnippetCallerCompatibility(impl.module,&impl.hook))
        throw std::runtime_error("Private runtime caller compatibility unavailable");
    result=callSnippetInitSafely(init,data.c_str(),session->device.Get(),&seh);
    checkNgx("Feature 18 Init_Ext",result,seh); impl.snippet=true;
    sessions[key]=session; writeDiagnosticLog(session->identity); return session;
}
NgxSession::~NgxSession() {
    DWORD seh=0;
    if (impl->snippet) callShutdownSafely(impl->shutdown,device.Get(),&seh);
    const bool restored=restoreSnippetCallerCompatibility(impl->hook);
    if (impl->module && restored) FreeLibrary(impl->module);
    if (impl->core) callShutdownSafely(&NVSDK_NGX_D3D12_Shutdown1,device.Get(),&seh);
}
void NgxSession::create(ID3D12GraphicsCommandList* list,Extent e,NVSDK_NGX_Parameter** parameters,NVSDK_NGX_Handle** handle) {
    DWORD seh=0; auto result=callAllocateParametersSafely(parameters,&seh); checkNgx("Allocate NR parameters",result,seh);
    if (!*parameters) throw std::runtime_error("Allocate NR returned null parameters");
    if (!setCreateParametersSafely(*parameters,e.width,e.height,1,&seh)) checkNgx("Set NR create parameters",NVSDK_NGX_Result_FAIL_PlatformError,seh);
    result=callCreateFeatureSafely(impl->create,list,*parameters,handle,&seh); checkNgx("Create Feature 18",result,seh);
    if (!*handle) throw std::runtime_error("Create NR returned null feature");
}
void NgxSession::evaluate(ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* parameters,NVSDK_NGX_Handle* handle,
    ID3D12Resource* input,ID3D12Resource* output,ID3D12Resource* motion,ID3D12Resource* depth,Extent e,
    const PassSettings& p,bool reset) {
    resolve_dlss5::Feature18Settings settings;
    settings.style=p.style; settings.intensity=p.intensity; settings.localToneStrength=p.tone;
    settings.localStructureStrength=p.structure; settings.skinStructureStrength=p.skin;
    settings.useAutoMask=p.mask; settings.uiCorrection=p.ui;
    DWORD seh=0;
    if (!setEvaluateParametersSafely(parameters,input,output,motion,depth,e.width,e.height,&settings,reset,&seh))
        checkNgx("Set NR evaluate parameters",NVSDK_NGX_Result_FAIL_PlatformError,seh);
    const auto result=callEvaluateFeatureSafely(impl->evaluate,list,handle,parameters,&seh);
    checkNgx("Evaluate Feature 18",result,seh);
}
void NgxSession::release(NVSDK_NGX_Parameter* parameters,NVSDK_NGX_Handle* handle) noexcept {
    std::scoped_lock lock(execution); DWORD seh=0;
    if (handle && impl->release) callReleaseFeatureSafely(impl->release,handle,&seh);
    if (parameters) callDestroyParametersSafely(parameters,&seh);
}
} // namespace neural
