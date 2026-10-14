// SPDX-License-Identifier: GPL-3.0-only
#pragma once
extern "C" {
#include <obs-module.h>
}
#include <graphics/vec4.h>
#include <string>

// Resolve by capability so neither the PE import table nor obs_module_ver
// introduces a numeric OBS-version prerequisite.
#define NEURAL_OBS_REQUIRED(X) \
 X(blog) X(obs_register_source_s) X(obs_enter_graphics) X(obs_leave_graphics) \
 X(obs_filter_get_target) X(obs_filter_get_parent) X(obs_source_get_width) X(obs_source_get_height) \
 X(obs_source_get_output_flags) X(obs_source_video_render) X(obs_source_default_render) X(obs_source_skip_video_filter) \
 X(obs_source_get_signal_handler) X(obs_source_get_weak_source) X(obs_weak_source_get_source) \
 X(obs_weak_source_release) X(obs_source_release) X(signal_handler_connect) X(signal_handler_disconnect) \
 X(obs_data_get_int) X(obs_data_get_double) X(obs_data_get_bool) X(obs_data_get_string) \
 X(obs_data_set_default_int) X(obs_data_set_default_double) X(obs_data_set_default_bool) X(obs_data_set_default_string) \
 X(obs_properties_create) X(obs_properties_add_bool) X(obs_properties_add_int_slider) \
 X(obs_properties_add_float_slider) X(obs_properties_add_list) X(obs_properties_add_text) \
 X(obs_properties_add_path) X(obs_properties_add_group) X(obs_properties_add_button2) \
 X(obs_property_list_add_int) X(obs_property_set_modified_callback) X(obs_property_set_visible) \
 X(obs_properties_get) X(obs_property_set_description) X(obs_property_set_long_description) \
 X(gs_get_device_type) X(gs_get_device_obj) X(gs_texture_get_obj) X(gs_texture_create) X(gs_texture_destroy) \
 X(gs_texrender_create) X(gs_texrender_destroy) X(gs_texrender_reset) X(gs_texrender_begin_with_color_space) \
 X(gs_texrender_end) X(gs_texrender_get_texture) X(gs_effect_create_from_file) X(gs_effect_destroy) \
 X(gs_effect_get_param_by_name) X(gs_effect_set_texture) X(gs_effect_loop) X(gs_draw_sprite) \
 X(gs_blend_state_push) X(gs_blend_state_pop) X(gs_blend_function_separate) X(gs_blend_function) \
 X(gs_enable_blending) X(gs_clear) X(gs_ortho) X(gs_get_color_space) \
 X(gs_framebuffer_srgb_enabled) X(gs_enable_framebuffer_srgb)
#define NEURAL_OBS_OPTIONAL(X) X(obs_source_get_color_space) X(obs_get_video_frame_time) X(obs_get_version_string) X(obs_get_module_data_path) X(obs_source_media_get_time) \
 X(obs_property_float_set_suffix) X(obs_source_update_properties) X(gs_get_viewport) X(gs_get_render_target)
namespace neural {
struct ObsApi {
#define DECLARE(name) decltype(&::name) name=nullptr;
    NEURAL_OBS_REQUIRED(DECLARE)
    NEURAL_OBS_OPTIONAL(DECLARE)
#undef DECLARE
    bool bind(std::string& reason);
    using Resolver=void*(*)(const char*);
    bool bind(Resolver resolver,std::string& reason);
};
extern ObsApi api;
const char* text(const char* key);
std::string dataDirectory();
void loadLocale(const char* locale);
void registerFilter();
}
