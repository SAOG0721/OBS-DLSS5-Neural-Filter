// SPDX-License-Identifier: GPL-3.0-only
#include "ObsApi.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
extern "C" uint32_t obs_module_ver();
namespace neural { void registerFilter() {} void warmShaderBytecode() {} }
namespace {
std::set<std::string> missing;
void placeholder() {}
void* resolve(const char* name) { return missing.contains(name) ? nullptr : reinterpret_cast<void*>(&placeholder); }
unsigned canvasRequests=0,canvasReleases=0,globalRequests=0;
bool canvasAvailable=true,canvasInfoValid=true;
uint32_t canvasNumerator=24000,canvasDenominator=1001,globalNumerator=60000,globalDenominator=1001;
obs_canvas_t* sourceCanvas(const obs_source_t*) {
    ++canvasRequests;
    return canvasAvailable ? reinterpret_cast<obs_canvas_t*>(uintptr_t{2}) : nullptr;
}
bool canvasVideoInfo(const obs_canvas_t*,obs_video_info* info) {
    info->fps_num=canvasNumerator; info->fps_den=canvasDenominator; return canvasInfoValid;
}
void releaseCanvas(obs_canvas_t*) { ++canvasReleases; }
bool globalVideoInfo(obs_video_info* info) {
    ++globalRequests; info->fps_num=globalNumerator; info->fps_den=globalDenominator; return true;
}
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    neural::ObsApi api; std::string error;
    if (!api.bind(resolve,error)) throw std::runtime_error(error);
    for (const char* capability : {"gs_get_device_obj","gs_texture_get_obj","obs_register_source_s","obs_filter_get_target","obs_data_get_double"}) {
        missing={capability};
        if (api.bind(resolve,error) || error!=capability) throw std::runtime_error("Required capability admitted: "+std::string(capability));
    }
    missing={"obs_get_version_string","obs_get_video_frame_time","obs_source_get_color_space","obs_get_video_info",
             "obs_source_get_canvas","obs_canvas_get_video_info","obs_canvas_release",
             "obs_property_visible","obs_property_description","obs_property_long_description","obs_property_text_info_type",
             "obs_property_int_set_suffix","obs_property_list_item_disable","obs_property_list_item_disabled",
             "obs_property_text_set_info_type","obs_property_text_set_info_word_wrap"};
    if (!api.bind(resolve,error)) throw std::runtime_error("Optional metadata used as load gate");
    if (obs_module_ver()!=0) throw std::runtime_error("OBS numeric host-loader gate reintroduced");
    neural::ObsApi videoApi;
    videoApi.obs_source_get_canvas=sourceCanvas; videoApi.obs_canvas_get_video_info=canvasVideoInfo;
    videoApi.obs_canvas_release=releaseCanvas; videoApi.obs_get_video_info=globalVideoInfo;
    auto* owner=reinterpret_cast<obs_source_t*>(uintptr_t{1});
    check(std::abs(videoApi.videoTargetFps(owner)-24000.f/1001)<.0001f && canvasReleases==1 && globalRequests==0,
          "Owning-canvas FPS was not preferred and released");
    canvasInfoValid=false;
    check(std::abs(videoApi.videoTargetFps(owner)-60000.f/1001)<.0001f && canvasReleases==2 && globalRequests==1,
          "Unavailable canvas info did not fall back to global video configuration");
    canvasInfoValid=true; canvasDenominator=0;
    check(std::abs(videoApi.videoTargetFps(owner)-60000.f/1001)<.0001f && canvasReleases==3,
          "Invalid canvas FPS did not fall back or leaked its reference");
    videoApi.obs_canvas_release=nullptr;
    const auto beforeRequests=canvasRequests;
    check(std::abs(videoApi.videoTargetFps(owner)-60000.f/1001)<.0001f && canvasRequests==beforeRequests,
          "Incomplete canvas API group acquired an unreleasable reference");
    globalDenominator=0;
    check(videoApi.videoTargetFps(owner)==60,"Unavailable video configuration has no stable fallback");
    videoApi.obs_get_video_info=nullptr;
    check(videoApi.videoTargetFps(nullptr)==60,"Missing optional FPS APIs changed capability admission behavior");
    std::cout << "Capability admission and numeric gate contracts passed\n";
}
