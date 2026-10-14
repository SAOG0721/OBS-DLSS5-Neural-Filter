// SPDX-License-Identifier: GPL-3.0-only
#include "ObsApi.h"
#include "GpuBackend.h"
#include <Windows.h>
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
    const auto name=requestedLocale.starts_with("zh") ? "zh-CN" : "en-US";
    const auto directory=dataDirectory();
    std::ifstream file(std::filesystem::path(std::u8string(directory.begin(),directory.end()))/"locale"/(std::string(name)+".ini"));
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
