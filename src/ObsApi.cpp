// SPDX-License-Identifier: GPL-3.0-only
#include "ObsApi.h"
#include "GpuBackend.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace neural {
ObsApi api;
namespace {
obs_module_t* module=nullptr;
std::string requestedLocale="en-US";
std::map<std::string,std::string> strings;
void* resolve(const char* name) {
    const auto obs=GetModuleHandleW(L"obs.dll");
    return obs ? reinterpret_cast<void*>(GetProcAddress(obs,name)) : nullptr;
}
}
bool ObsApi::bind(std::string& reason) { return bind(&resolve,reason); }
bool ObsApi::bind(Resolver resolver,std::string& reason) {
    reason.clear();
#define BIND(name) name=reinterpret_cast<decltype(name)>(resolver(#name)); if (!name) { if (!reason.empty()) reason+=", "; reason+=#name; }
    NEURAL_OBS_REQUIRED(BIND)
#undef BIND
#define BIND(name) name=reinterpret_cast<decltype(name)>(resolver(#name));
    NEURAL_OBS_OPTIONAL(BIND)
#undef BIND
    return reason.empty();
}
float ObsApi::videoTargetFps(const obs_source_t* owner) const {
    obs_video_info info{};
    // A source's owning canvas can have a different rate from the main canvas.
    // The getter increments the canvas reference, so use it only with release.
    if (owner && obs_source_get_canvas && obs_canvas_get_video_info && obs_canvas_release) {
        if (auto* canvas=obs_source_get_canvas(owner)) {
            const bool valid=obs_canvas_get_video_info(canvas,&info);
            obs_canvas_release(canvas);
            if (valid && info.fps_num && info.fps_den)
                return static_cast<float>(info.fps_num)/info.fps_den;
        }
    }
    if (obs_get_video_info && obs_get_video_info(&info) && info.fps_num && info.fps_den)
        return static_cast<float>(info.fps_num)/info.fps_den;
    return 60;
}
std::string dataDirectory() {
    if (module && api.obs_get_module_data_path) return api.obs_get_module_data_path(module);
    HMODULE current=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&dataDirectory),&current);
    std::wstring file(32768,L'\0');
    const auto count=GetModuleFileNameW(current,file.data(),static_cast<DWORD>(file.size()));
    file.resize(count);
    const auto directory=std::filesystem::path(file).parent_path().parent_path().parent_path()/L"data";
    const auto utf8=directory.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()),utf8.size());
}
void loadLocale(const char* locale) {
    requestedLocale=locale ? locale : "en-US";
    if (!api.obs_get_module_data_path && !module) return;
    strings.clear();
    std::string lower=requestedLocale;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool traditional=lower.starts_with("zh-tw") || lower.starts_with("zh_tw") ||
        lower.starts_with("zh-hk") || lower.starts_with("zh_hk") ||
        lower.starts_with("zh-mo") || lower.starts_with("zh_mo") ||
        lower.starts_with("zh-hant") || lower.starts_with("zh_hant");
    const auto directory=dataDirectory();
    const auto openLocale=[&](const char* name) {
        return std::ifstream(std::filesystem::path(std::u8string(directory.begin(),directory.end()))/"locale"/(std::string(name)+".ini"));
    };
    auto file=openLocale(traditional ? "zh-TW" : lower.starts_with("zh") ? "zh-CN" : "en-US");
    if (!file && traditional) file=openLocale("zh-CN");
    std::string line;
    while (std::getline(file,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        const auto equals=line.find('=');
        if (equals==std::string::npos || line.empty() || line[0]=='#') continue;
        auto value=line.substr(equals+1);
        if (value.size()>=2 && value.front()=='"' && value.back()=='"') value=value.substr(1,value.size()-2);
        strings[line.substr(0,equals)]=std::move(value);
    }
}
const char* text(const char* key) { const auto i=strings.find(key); return i == strings.end() ? key : i->second.c_str(); }
}
extern "C" {
__declspec(dllexport) void obs_module_set_pointer(obs_module_t* value) { neural::module=value; }
// OBS itself rejects a higher advertised ABI number before calling load. Zero
// deliberately disables that numeric gate; required public APIs are resolved
// and checked before any source registration instead.
__declspec(dllexport) uint32_t obs_module_ver(void) { return 0; }
__declspec(dllexport) const char* obs_module_name(void) { return "DLSS5 Neural Filter"; }
__declspec(dllexport) const char* obs_module_description(void) { return "GPU neural rendering for a single OBS source"; }
__declspec(dllexport) const char* obs_module_author(void) { return "DLSS5 Neural Filter contributors"; }
__declspec(dllexport) void obs_module_set_locale(const char* locale) { neural::loadLocale(locale); }
__declspec(dllexport) void obs_module_free_locale(void) {}
__declspec(dllexport) bool obs_module_load(void) {
    std::string reason;
    if (!neural::api.bind(reason)) {
        const auto message="[DLSS5 Neural Filter] missing capabilities: "+reason;
        OutputDebugStringA(message.c_str());
        if (neural::api.blog) neural::api.blog(LOG_WARNING,"%s",message.c_str());
        return false;
    }
    neural::loadLocale(neural::requestedLocale.c_str());
    neural::api.blog(LOG_INFO,"[DLSS5 Neural Filter] %s; host=%s; capability-based admission; no OBS version gate",
        NEURAL_PLUGIN_VERSION,neural::api.obs_get_version_string ? neural::api.obs_get_version_string() : "unreported");
    try { neural::warmShaderBytecode(); }
    catch (const std::exception& e) { neural::api.blog(LOG_WARNING,"[DLSS5 Neural Filter] Shader prewarm: %s",e.what()); }
    neural::registerFilter(); return true;
}
}
