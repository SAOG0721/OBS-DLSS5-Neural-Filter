// SPDX-License-Identifier: GPL-3.0-only
#include "ObsApi.h"
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
extern "C" uint32_t obs_module_ver();
namespace neural { void registerFilter() {} void warmShaderBytecode() {} }
namespace {
std::set<std::string> missing;
void placeholder() {}
void* resolve(const char* name) { return missing.contains(name) ? nullptr : reinterpret_cast<void*>(&placeholder); }
}
int main() {
    neural::ObsApi api; std::string error;
    if (!api.bind(resolve,error)) throw std::runtime_error(error);
    for (const char* capability : {"gs_get_device_obj","gs_texture_get_obj","obs_register_source_s","obs_filter_get_target","obs_data_get_double"}) {
        missing={capability};
        if (api.bind(resolve,error) || error!=capability) throw std::runtime_error("Required capability admitted: "+std::string(capability));
    }
    missing={"obs_get_version_string","obs_get_video_frame_time","obs_source_get_color_space"};
    if (!api.bind(resolve,error)) throw std::runtime_error("Optional metadata used as load gate");
    if (obs_module_ver()!=0) throw std::runtime_error("OBS numeric host-loader gate reintroduced");
    std::cout << "Capability admission and numeric gate contracts passed\n";
}
