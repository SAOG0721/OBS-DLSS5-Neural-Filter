// SPDX-License-Identifier: GPL-3.0-only
// Actual libobs + image/media sources + production plugin; isolated from user settings.
#include "ObsApi.h"
#include <Windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <chrono>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
void require(bool okay,const std::string& message) { if (!okay) throw std::runtime_error(message); }
template<class T> T bind(HMODULE module,const char* name) {
    auto p=reinterpret_cast<T>(GetProcAddress(module,name)); require(p!=nullptr,std::string("Missing harness capability: ")+name); return p;
}
std::vector<unsigned short> read(ID3D11Device* device,ID3D11Texture2D* original) {
    D3D11_TEXTURE2D_DESC d{}; original->GetDesc(&d);
    d.BindFlags=0; d.MiscFlags=0; d.Usage=D3D11_USAGE_STAGING; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Create OBS readback");
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    context->CopyResource(staging.Get(),original); context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    HRESULT result;
    do { result=context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
         if (result==DXGI_ERROR_WAS_STILL_DRAWING) Sleep(1);
    } while (result==DXGI_ERROR_WAS_STILL_DRAWING && std::chrono::steady_clock::now()<deadline);
    require(SUCCEEDED(result),"Bounded OBS output readback failed");
    std::vector<unsigned short> pixels(static_cast<size_t>(d.Width)*d.Height*4);
    for (unsigned y=0;y<d.Height;++y) std::memcpy(pixels.data()+static_cast<size_t>(y)*d.Width*4,
        static_cast<unsigned char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,d.Width*8);
    context->Unmap(staging.Get(),0); return pixels;
}
int main(int argc,char** argv) {
    try {
        require(argc>=5,"Usage: obs_harness OBS-runtime-root plugin-path input-path runtime-DLL [output-dir] [mode] [plugin-data] [settings-JSON]");
        const bool identity=argc>6 && std::string(argv[6])=="--identity";
        const bool monitor=argc>6 && std::string(argv[6])=="--monitor";
        const bool dual=argc>6 && std::string(argv[6])=="--dual-preview";
        const bool runtimeSwitch=argc>6 && std::string(argv[6])=="--runtime-switch";
        const bool steady=argc>6 && std::string(argv[6])=="--steady-1440p";
        const bool recovery=argc>6 && std::string(argv[6])=="--record-recovery";
        const bool media=argc>6 && (std::string(argv[6])=="--media" || std::string(argv[6])=="--media-record");
        const bool record=argc>6 && (std::string(argv[6])=="--record" || std::string(argv[6])=="--media-record" || recovery);
        const std::string runtimeSelection=std::string(argv[4])=="--private-runtime" ? "" :
            std::filesystem::absolute(argv[4]).string();
        std::filesystem::path alternateRuntimePath;
        std::string alternateRuntime;
        if (runtimeSwitch) {
            std::array<wchar_t,32768> value{};
            const auto length=GetEnvironmentVariableW(L"NEURAL_HARNESS_ALTERNATE_RUNTIME",value.data(),static_cast<DWORD>(value.size()));
            require(length>0 && length<value.size(),"--runtime-switch needs NEURAL_HARNESS_ALTERNATE_RUNTIME set to an absolute runtime DLL path");
            alternateRuntimePath=std::filesystem::path(value.data());
            require(alternateRuntimePath.is_absolute() && std::filesystem::is_regular_file(alternateRuntimePath),
                    "Alternate runtime must be an existing absolute DLL path");
            alternateRuntimePath=std::filesystem::weakly_canonical(alternateRuntimePath);
            const auto utf8=alternateRuntimePath.u8string();
            alternateRuntime.assign(utf8.begin(),utf8.end());
        }
        const auto root=std::filesystem::absolute(argv[1]);
        const auto binary=root/"bin"/"64bit";
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const auto cookie=AddDllDirectory(binary.c_str()); require(cookie!=nullptr,"Add OBS runtime directory");
        const auto dll=LoadLibraryExW((binary/"obs.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(dll!=nullptr,"Load actual OBS library");
#define B(name) auto p_##name=bind<decltype(&::name)>(dll,#name);
        B(obs_startup) B(obs_shutdown) B(obs_add_data_path) B(obs_reset_video) B(obs_open_module) B(obs_init_module)
        B(obs_source_create) B(obs_source_release) B(obs_source_update) B(obs_source_set_enabled)
        B(obs_source_duplicate)
        B(obs_source_media_play_pause) B(obs_source_media_restart) B(obs_source_media_get_time)
        B(obs_source_filter_set_order)
        B(obs_source_filter_add) B(obs_source_filter_remove) B(obs_source_properties) B(obs_source_get_settings)
        B(obs_source_get_width) B(obs_source_get_height) B(obs_source_video_render) B(obs_source_inc_showing) B(obs_source_dec_showing)
        B(obs_data_create) B(obs_data_release) B(obs_data_set_string) B(obs_data_set_double) B(obs_data_set_int) B(obs_data_set_bool)
        B(obs_data_get_int) B(obs_data_get_double) B(obs_data_get_bool) B(obs_data_get_string) B(obs_data_get_json) B(obs_data_create_from_json)
        B(obs_properties_destroy) B(obs_properties_get) B(obs_properties_apply_settings) B(obs_property_modified) B(obs_property_button_clicked)
        B(obs_property_description) B(obs_property_long_description) B(obs_property_visible)
        B(obs_reset_audio) B(obs_get_video) B(obs_get_audio) B(obs_set_output_source)
        B(obs_video_encoder_create) B(obs_audio_encoder_create) B(obs_encoder_release)
        B(obs_encoder_set_video) B(obs_encoder_set_audio)
        B(obs_output_create) B(obs_output_release) B(obs_output_set_video_encoder) B(obs_output_set_audio_encoder)
        B(obs_output_start) B(obs_output_stop) B(obs_output_active)
        B(obs_output_get_total_frames) B(obs_output_get_frames_dropped) B(obs_output_get_last_error)
        B(obs_enter_graphics) B(obs_leave_graphics) B(gs_texrender_create) B(gs_texrender_destroy)
        B(gs_texrender_reset) B(gs_texrender_begin_with_color_space) B(gs_texrender_end) B(gs_texrender_get_texture)
        B(gs_clear) B(gs_ortho) B(gs_blend_state_push) B(gs_blend_state_pop) B(gs_blend_function)
        B(gs_get_device_obj) B(gs_texture_get_obj)
#undef B
        require(p_obs_startup("en-US",nullptr,nullptr),"OBS startup failed");
        p_obs_add_data_path(((root/"data"/"libobs").string()+"/").c_str());
        obs_video_info info{}; const auto graphics=(binary/"libobs-d3d11.dll").string();
        info.graphics_module=graphics.c_str(); info.base_width=(monitor || steady) ? 2560 : 1280; info.base_height=(monitor || steady) ? 1440 : 720;
        info.output_width=info.base_width; info.output_height=info.base_height; info.fps_num=60; info.fps_den=1;
        info.output_format=(record || monitor || steady) ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_RGBA;
        info.gpu_conversion=record || monitor || steady; info.colorspace=VIDEO_CS_709;
        info.range=record ? VIDEO_RANGE_PARTIAL : VIDEO_RANGE_FULL;
        require(p_obs_reset_video(&info)==OBS_VIDEO_SUCCESS,"OBS video initialization failed");
        auto module=[&](const std::filesystem::path& file,const std::filesystem::path& data) {
            obs_module_t* m=nullptr;
            require(p_obs_open_module(&m,file.string().c_str(),data.string().c_str())==MODULE_SUCCESS,"OBS open module failed: "+file.string());
            require(p_obs_init_module(m),"OBS module initialization failed: "+file.string());
        };
        module(root/"obs-plugins"/"64bit"/"image-source.dll",root/"data"/"obs-plugins"/"image-source");
        module(root/"obs-plugins"/"64bit"/"obs-filters.dll",root/"data"/"obs-plugins"/"obs-filters");
        if (monitor) module(root/"obs-plugins"/"64bit"/"win-capture.dll",root/"data"/"obs-plugins"/"win-capture");
        if (record || media) {
            obs_audio_info audio{}; audio.samples_per_sec=48000; audio.speakers=SPEAKERS_STEREO;
            require(p_obs_reset_audio(&audio),"OBS audio initialization failed");
            module(root/"obs-plugins"/"64bit"/"obs-ffmpeg.dll",root/"data"/"obs-plugins"/"obs-ffmpeg");
            if (record) module(root/"obs-plugins"/"64bit"/"obs-nvenc.dll",root/"data"/"obs-plugins"/"obs-nvenc");
        }
        const auto pluginData=argc>7 ? std::filesystem::absolute(argv[7]) :
            std::filesystem::absolute(argv[2]).parent_path().parent_path().parent_path()/"data";
        module(std::filesystem::absolute(argv[2]),pluginData);
        auto* imageSettings=p_obs_data_create();
        p_obs_data_set_string(imageSettings,monitor ? "monitor_id" : media ? "local_file" : "file",monitor ? argv[3] : std::filesystem::absolute(argv[3]).string().c_str());
        if (media) {
            // Public obs-ffmpeg ffmpeg_source settings, verified in the local OBS source.
            p_obs_data_set_bool(imageSettings,"is_local_file",true); p_obs_data_set_bool(imageSettings,"looping",true);
            p_obs_data_set_bool(imageSettings,"restart_on_activate",false); p_obs_data_set_bool(imageSettings,"close_when_inactive",false);
            p_obs_data_set_bool(imageSettings,"clear_on_media_end",false); p_obs_data_set_bool(imageSettings,"hw_decode",false);
        }
        auto* image=p_obs_source_create(monitor ? "monitor_capture" : media ? "ffmpeg_source" : "image_source","fixture",imageSettings,nullptr);
        p_obs_data_release(imageSettings); require(image!=nullptr,"Create OBS fixture source"); p_obs_source_inc_showing(image);
        if (monitor || media) p_obs_set_output_source(0,image);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (!p_obs_source_get_width(image) && std::chrono::steady_clock::now()<deadline) Sleep(10);
        const unsigned width=p_obs_source_get_width(image),height=p_obs_source_get_height(image);
        require(width && height,"Fixture source has no pixels");
        obs_data_t* settings=nullptr;
        if (argc>8) {
            std::ifstream file(std::filesystem::absolute(argv[8]),std::ios::binary);
            require(file.good(),"Cannot open harness settings JSON");
            const std::string json((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            settings=p_obs_data_create_from_json(json.c_str());
            require(settings!=nullptr,"Cannot parse harness settings JSON");
        } else settings=p_obs_data_create();
        // Preserve the positional runtime override, including bundled-runtime selection.
        p_obs_data_set_string(settings,"runtime",runtimeSelection.c_str());
        auto* filter=p_obs_source_create("dlss5_neural_filter","DLSS5 Neural Filter",settings,nullptr);
        require(filter!=nullptr,"Create production source filter"); p_obs_source_filter_add(image,filter);
        const std::array<const char*,12> persistentInts{"schemaVersion","model_preset","resolution_control","processing_mode",
            "update_interval","residual_transfer","percent","flow","amd_quality","nv_quality","af","p2_style"};
        const std::array<const char*,3> persistentFloats{"midtone","chroma_protection","response_ms"};
        std::array<long long,12> expectedInts{}; std::array<double,3> expectedFloats{};
        auto* initialSettings=p_obs_source_get_settings(filter);
        for (size_t i=0;i<persistentInts.size();++i) expectedInts[i]=p_obs_data_get_int(initialSettings,persistentInts[i]);
        for (size_t i=0;i<persistentFloats.size();++i) expectedFloats[i]=p_obs_data_get_double(initialSettings,persistentFloats[i]);
        const std::string expectedUnknown=p_obs_data_get_string(initialSettings,"future_extension");
        const bool expectedScaling=p_obs_data_get_bool(initialSettings,"scaling");
        const auto expectedDebug=p_obs_data_get_int(initialSettings,"debug");
        require(std::string(p_obs_data_get_string(initialSettings,"runtime"))==runtimeSelection,"JSON overrode positional runtime selection");
        const std::string initialJson=p_obs_data_get_json(initialSettings);
        p_obs_data_release(initialSettings);
        const auto infoText=[&](obs_properties_t* props,const char* key) {
            auto* property=p_obs_properties_get(props,key);
            if (!property) return std::string();
            const auto* detail=p_obs_property_long_description(property);
            if (detail && *detail) return std::string(detail);
            const auto* desc=p_obs_property_description(property);
            return desc ? std::string(desc) : std::string();
        };
        if (monitor || steady) {
            p_obs_set_output_source(0,image);
            char duration[32]{}; const auto hasDuration=GetEnvironmentVariableA("NEURAL_HARNESS_SECONDS",duration,sizeof(duration));
            const int seconds=hasDuration ? std::clamp(std::stoi(duration),1,14400) : 30;
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
            std::string status;
            while (std::chrono::steady_clock::now()<end) {
                Sleep(500);
                auto* props=p_obs_source_properties(filter);
                status=infoText(props,"status");
                const auto runtime=infoText(props,"runtime_status");
                require(runtime.find("File found")!=std::string::npos,"Bundled runtime status did not find DLL");
                p_obs_properties_destroy(props);
                require(status.find("deviceRemovedReason")==std::string::npos && status.find("device removal")==std::string::npos,"Monitor processing lost GPU device: "+status);
            }
            require(status.find("Processing")!=std::string::npos || status.find("completed result")!=std::string::npos,"Source did not finish with a completed result: "+status);
            p_obs_set_output_source(0,nullptr);
            p_obs_source_filter_remove(image,filter); p_obs_source_release(filter); p_obs_data_release(settings);
            p_obs_source_dec_showing(image); p_obs_source_release(image); p_obs_shutdown();
            std::cout << (monitor ? "Real monitor_capture" : "Actual image_source") << " 2560x1440/60 NV12, bundled runtime and " << seconds << "-second processing passed: " << status << '\n';
            RemoveDllDirectory(cookie); return 0;
        }
        p_obs_enter_graphics(); auto* capture=p_gs_texrender_create(GS_RGBA16F,GS_ZS_NONE); p_obs_leave_graphics();
        auto captureSource=[&](obs_source_t* selected=nullptr,gs_color_space space=GS_CS_SRGB_16F) {
            p_obs_enter_graphics(); p_gs_texrender_reset(capture);
            require(p_gs_texrender_begin_with_color_space(capture,width,height,space),"Begin OBS test capture");
            vec4 clear{}; p_gs_clear(GS_CLEAR_COLOR,&clear,0,0);
            p_gs_ortho(0,static_cast<float>(width),0,static_cast<float>(height),-100,100);
            p_gs_blend_state_push(); p_gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
            p_obs_source_video_render(selected ? selected : image); p_gs_blend_state_pop(); p_gs_texrender_end(capture);
            auto* texture=static_cast<ID3D11Texture2D*>(p_gs_texture_get_obj(p_gs_texrender_get_texture(capture)));
            auto* device=static_cast<ID3D11Device*>(p_gs_get_device_obj());
            auto result=read(device,texture); p_obs_leave_graphics(); return result;
        };
        unsigned movingOriginalChanges=0,movingProcessedChanges=0;
        std::string mediaMetrics;
        if (media) {
            // Verify playback while moving, then freeze the same source for the
            // existing exact RGB/alpha/mix-zero and recovery contracts below.
            p_obs_source_set_enabled(filter,false); Sleep(100);
            auto previous=captureSource();
            for (int i=0;i<15;++i) { Sleep(34); auto current=captureSource(); if (current!=previous) ++movingOriginalChanges; previous=std::move(current); }
            require(movingOriginalChanges>=2,"Media source did not supply changing decoded frames");
            p_obs_source_set_enabled(filter,true); Sleep(200);
            const auto warmDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            std::string warmStatus;
            do {
                captureSource();
                auto* props=p_obs_source_properties(filter);
                warmStatus=infoText(props,"status");
                p_obs_properties_destroy(props);
                if (warmStatus.find("Processing")!=std::string::npos || warmStatus.find("completed result")!=std::string::npos) break;
                Sleep(20);
            } while (std::chrono::steady_clock::now()<warmDeadline);
            require(warmStatus.find("Processing")!=std::string::npos || warmStatus.find("completed result")!=std::string::npos,
                    "Media source NR initialization did not produce a completed result: "+warmStatus);
            previous=captureSource();
            for (int i=0;i<60;++i) { Sleep(17); auto current=captureSource(); if (current!=previous) ++movingProcessedChanges; previous=std::move(current); }
            require(movingProcessedChanges>=2,"Filtered media output did not update with the moving source");
            auto* props=p_obs_source_properties(filter);
            const std::string status=infoText(props,"status");
            mediaMetrics=infoText(props,"performance_status")+"<br>"+infoText(props,"performance_detail");
            p_obs_properties_destroy(props);
            require(status.find("Processing")!=std::string::npos || status.find("completed result")!=std::string::npos,
                    "Moving media source has no completed neural result: "+status);
            std::cout << "Actual ffmpeg_source motion: originalChanges=" << movingOriginalChanges
                      << "; processedChanges=" << movingProcessedChanges << "; mediaTimeMs=" << p_obs_source_media_get_time(image)
                      << "; canvas=60 FPS; " << mediaMetrics << '\n';
            p_obs_source_media_play_pause(image,true); Sleep(150); p_obs_set_output_source(0,nullptr);
        }
        p_obs_source_set_enabled(filter,false); Sleep(50);
        auto original=captureSource();
        if (media) {
            // Pausing the decoder leaves already queued asynchronous frames in
            // libobs. Drain them before taking a fixed-frame pixel reference.
            const auto settleDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            auto stableSince=std::chrono::steady_clock::now();
            unsigned settlingChanges=0;
            while (std::chrono::steady_clock::now()<settleDeadline &&
                   std::chrono::steady_clock::now()-stableSince<std::chrono::milliseconds(250)) {
                Sleep(34); auto current=captureSource();
                if (current!=original) { original=std::move(current); ++settlingChanges; stableSince=std::chrono::steady_clock::now(); }
            }
            require(std::chrono::steady_clock::now()-stableSince>=std::chrono::milliseconds(250),
                    "Paused media source did not settle to a stable unfiltered frame");
            std::cout << "Paused media baseline settled: drainedFrameChanges=" << settlingChanges << "; stableForMs>=250\n";
        }
        p_obs_source_set_enabled(filter,true); Sleep(50);
        auto captureProcessed=[&](obs_source_t* selected=nullptr) {
            auto result=captureSource(selected);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!identity && result==original && std::chrono::steady_clock::now()<deadline) {
                Sleep(10); result=captureSource(selected);
            }
            return result;
        };
        const auto processed=captureProcessed();
        if (dual) {
            for (int frame=0;frame<90;++frame) {
                const auto mainView=captureSource(nullptr,GS_CS_SRGB_16F);
                const auto filterView=captureSource(nullptr,GS_CS_SRGB);
                std::size_t nonzero=0;
                for (size_t i=0;i<filterView.size();i+=4) {
                    if (filterView[i] || filterView[i+1] || filterView[i+2]) ++nonzero;
                    require(filterView[i+3]==mainView[i+3],"Two preview spaces changed alpha");
                }
                require(nonzero>0,"Filter preview is black");
                require(filterView!=original && mainView!=original,"Two-view drawing returned raw input during processing");
                Sleep(17);
            }
            std::cout << "Main FP16 and filter SDR preview render paths: 90 pairs visible, processed, alpha retained\n";
        }
        std::size_t changes=0,alphaMismatch=0;
        for (size_t i=0;i<original.size();i+=4) {
            if (original[i]!=processed[i] || original[i+1]!=processed[i+1] || original[i+2]!=processed[i+2]) ++changes;
            if (original[i+3]!=processed[i+3]) ++alphaMismatch;
        }
        if (identity) require(processed==original,"Actual OBS identity roundtrip changed RGB or alpha");
        else require(changes>0,"Actual OBS normal view has no NR correction");
        require(alphaMismatch==0,"Actual OBS filter chain changed alpha");
        p_obs_data_set_double(settings,"mix",0); p_obs_source_update(filter,settings); Sleep(50);
        const auto mixZero=captureSource();
        if (media && mixZero!=original) {
            p_obs_source_set_enabled(filter,false); Sleep(50);
            const auto currentOriginal=captureSource();
            size_t originalDifferences=0,currentDifferences=0;
            for (size_t i=0;i<original.size();++i) {
                originalDifferences+=mixZero[i]!=original[i];
                currentDifferences+=mixZero[i]!=currentOriginal[i];
            }
            std::cerr << "Media mix-zero evidence: versusStoredOriginalComponents=" << originalDifferences
                      << "; versusCurrentDisabledComponents=" << currentDifferences
                      << "; mediaTimeMs=" << p_obs_source_media_get_time(image) << '\n';
            if (argc>5) {
                const auto out=std::filesystem::absolute(argv[5]); std::filesystem::create_directories(out);
                const auto save=[&](const char* name,const std::vector<unsigned short>& pixels) {
                    std::ofstream file(out/name,std::ios::binary);
                    file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*2);
                };
                save("failure-stored-original.rgba16f",original); save("failure-mix-zero.rgba16f",mixZero);
                save("failure-current-disabled.rgba16f",currentOriginal);
                std::ofstream(out/"failure-dimensions.txt") << width << ' ' << height;
            }
        }
        require(mixZero==original,"OBS mix-zero did not restore original exactly");
        p_obs_data_set_double(settings,"mix",1); p_obs_data_set_int(settings,"passes",2);
        p_obs_source_update(filter,settings); Sleep(50); captureSource();
        p_obs_data_set_int(settings,"passes",3);
        p_obs_data_set_double(settings,"p3_tone",1.25);
        p_obs_source_update(filter,settings); Sleep(50); captureSource();
        p_obs_data_set_int(settings,"passes",1); p_obs_source_update(filter,settings); Sleep(50); captureSource();
        auto* persisted=p_obs_source_get_settings(filter);
        const std::string json=p_obs_data_get_json(persisted);
        auto* restored=p_obs_data_create_from_json(json.c_str());
        require(p_obs_data_get_int(restored,"passes")==1,"Scene JSON lost pass count");
        auto* restoredFilter=p_obs_source_create("dlss5_neural_filter","restored settings contract",restored,nullptr);
        require(restoredFilter!=nullptr,"Create filter from serialized settings");
        auto* effectiveRestored=p_obs_source_get_settings(restoredFilter);
        for (size_t i=0;i<persistentInts.size();++i)
            require(p_obs_data_get_int(effectiveRestored,persistentInts[i])==expectedInts[i],std::string("Scene JSON changed preserved setting: ")+persistentInts[i]);
        for (size_t i=0;i<persistentFloats.size();++i)
            require(p_obs_data_get_double(effectiveRestored,persistentFloats[i])==expectedFloats[i],std::string("Scene JSON changed preserved setting: ")+persistentFloats[i]);
        require(p_obs_data_get_bool(effectiveRestored,"scaling")==expectedScaling,"Scene JSON changed legacy scaling flag");
        require(std::string(p_obs_data_get_string(effectiveRestored,"future_extension"))==expectedUnknown,"Scene JSON removed an unknown future setting");
        p_obs_data_release(effectiveRestored); p_obs_source_release(restoredFilter);
        p_obs_data_release(persisted); p_obs_data_release(restored);
        auto* duplicate=p_obs_source_duplicate(image,"independent fixture",false);
        require(duplicate!=nullptr,"Source duplicate failed");
        // ffmpeg_source declares DO_NOT_DUPLICATE and returns another reference;
        // image-source invocations continue to check a distinct filtered source.
        require(media ? duplicate==image : duplicate!=image,"Unexpected OBS source duplication contract");
        p_obs_source_inc_showing(duplicate); Sleep(60);
        const auto second=captureProcessed(duplicate);
        require(identity ? second==original : second!=original,"Duplicated source lost filter settings or output");
        p_obs_source_dec_showing(duplicate); p_obs_source_release(duplicate);
        Sleep(60); captureSource();
        p_obs_source_dec_showing(image); Sleep(50); p_obs_source_inc_showing(image); Sleep(50);
        const auto resumed=captureProcessed();
        require(identity ? resumed==original : resumed!=original,"Hide/resume did not restore filter output");
        auto* colorSettings=p_obs_data_create();
        p_obs_data_set_double(colorSettings,"brightness",.15);
        auto* colorFilter=p_obs_source_create("color_filter_v2","upstream color",colorSettings,nullptr);
        require(colorFilter!=nullptr,"Create native color filter");
        p_obs_source_filter_add(image,colorFilter); Sleep(60);
        if (identity) {
            p_obs_source_set_enabled(filter,false); Sleep(50); const auto colorOriginal=captureSource();
            p_obs_source_set_enabled(filter,true); Sleep(50);
            require(captureSource()==colorOriginal,"Identity filter changed native-filter chain output");
            p_obs_source_filter_set_order(image,filter,OBS_ORDER_MOVE_DOWN); Sleep(60);
            require(captureSource()==colorOriginal,"Moving identity filter changed chain output");
        } else {
            captureSource();
            p_obs_source_filter_set_order(image,filter,OBS_ORDER_MOVE_DOWN); Sleep(60);
            captureSource();
        }
        p_obs_source_filter_remove(image,colorFilter); p_obs_source_release(colorFilter);
        p_obs_data_release(colorSettings); Sleep(60);
        if (!identity && (!record || recovery)) {
            p_obs_data_set_string(settings,"runtime","C:\\missing-neural-runtime.dll"); p_obs_source_update(filter,settings); Sleep(50);
            require(captureSource()==original,"Missing runtime did not return current original");
            p_obs_data_set_string(settings,"runtime",runtimeSelection.c_str());
            p_obs_source_update(filter,settings); Sleep(50);
            const auto recovered=captureProcessed(); require(recovered!=original,"Runtime path change did not recover");
        }
        std::string runtimeSwitchMetrics;
        if (runtimeSwitch) {
#define B(name) auto p_##name=bind<decltype(&::name)>(dll,#name);
            B(obs_data_first) B(obs_data_item_byname) B(obs_data_item_next) B(obs_data_item_release)
            B(obs_data_item_gettype) B(obs_data_item_numtype) B(obs_data_item_get_name)
            B(obs_data_get_obj) B(obs_data_get_array)
            B(obs_data_array_count) B(obs_data_array_item) B(obs_data_array_release)
#undef B
            // Compare the serialized user settings, including unknown nested
            // fields, without treating object key order as a value change.
            const auto sameValues=[&](auto&& self,obs_data_t* left,obs_data_t* right)->bool {
                if (!left || !right) return left==right;
                bool same=true;
                size_t leftCount=0,rightCount=0;
                for (auto* item=p_obs_data_first(left);item;p_obs_data_item_next(&item)) {
                    ++leftCount;
                    const auto* name=p_obs_data_item_get_name(item);
                    auto* other=p_obs_data_item_byname(right,name);
                    const auto type=p_obs_data_item_gettype(item);
                    bool equal=other && p_obs_data_item_gettype(other)==type;
                    if (equal) switch (type) {
                    case OBS_DATA_NULL: break;
                    case OBS_DATA_STRING:
                        equal=std::strcmp(p_obs_data_get_string(left,name),p_obs_data_get_string(right,name))==0; break;
                    case OBS_DATA_BOOLEAN:
                        equal=p_obs_data_get_bool(left,name)==p_obs_data_get_bool(right,name); break;
                    case OBS_DATA_NUMBER: {
                        const auto number=p_obs_data_item_numtype(item);
                        equal=number==p_obs_data_item_numtype(other) &&
                            (number==OBS_DATA_NUM_INT ? p_obs_data_get_int(left,name)==p_obs_data_get_int(right,name) :
                             number==OBS_DATA_NUM_DOUBLE && p_obs_data_get_double(left,name)==p_obs_data_get_double(right,name));
                        break;
                    }
                    case OBS_DATA_OBJECT: {
                        auto* a=p_obs_data_get_obj(left,name); auto* b=p_obs_data_get_obj(right,name);
                        equal=self(self,a,b);
                        p_obs_data_release(a); p_obs_data_release(b); break;
                    }
                    case OBS_DATA_ARRAY: {
                        auto* a=p_obs_data_get_array(left,name); auto* b=p_obs_data_get_array(right,name);
                        const auto count=p_obs_data_array_count(a);
                        equal=count==p_obs_data_array_count(b);
                        for (size_t i=0;equal && i<count;++i) {
                            auto* av=p_obs_data_array_item(a,i); auto* bv=p_obs_data_array_item(b,i);
                            equal=self(self,av,bv); p_obs_data_release(av); p_obs_data_release(bv);
                        }
                        p_obs_data_array_release(a); p_obs_data_array_release(b); break;
                    }
                    default: equal=false; break;
                    }
                    p_obs_data_item_release(&other); same=same && equal;
                }
                for (auto* item=p_obs_data_first(right);item;p_obs_data_item_next(&item)) ++rightCount;
                return same && leftCount==rightCount;
            };
            auto* beforeRuntime=p_obs_source_get_settings(filter);
            const std::string beforeRuntimeJson=p_obs_data_get_json(beforeRuntime);
            p_obs_data_release(beforeRuntime);
            auto* expectedRuntimeValues=p_obs_data_create_from_json(beforeRuntimeJson.c_str());
            require(expectedRuntimeValues!=nullptr,"Cannot snapshot runtime-switch settings");
            const auto escapePath=[](const std::string& path) {
                std::string result;
                for (const char c : path) {
                    if (c=='<') result+="&lt;"; else if (c=='>') result+="&gt;";
                    else if (c=='&') result+="&amp;"; else result+=c;
                }
                return result;
            };
            struct RuntimeSnapshot { std::string status,resources,runtime; };
            const auto snapshot=[&] {
                auto* props=p_obs_source_properties(filter);
                require(props!=nullptr,"Runtime switch properties unavailable");
                RuntimeSnapshot result{infoText(props,"status"),infoText(props,"resource_status"),infoText(props,"runtime_status")};
                p_obs_properties_destroy(props);
                require(!result.resources.empty(),"Runtime switch needs the background resource status property");
                return result;
            };
            const auto confirmRuntime=[&](const char* label,const std::string& selection,const std::string& expectedDescription) {
                std::string pathSuffix;
                if (!selection.empty()) {
                    const auto utf8=std::filesystem::weakly_canonical(std::filesystem::path(
                        std::u8string(selection.begin(),selection.end()))).u8string();
                    pathSuffix="<br>"+escapePath(std::string(utf8.begin(),utf8.end()));
                }
                const auto ready=[&](const RuntimeSnapshot& value) {
                    return value.resources.find("Idle")!=std::string::npos &&
                        (value.status.find("Processing")!=std::string::npos || value.status.find("completed result")!=std::string::npos) &&
                        value.runtime.find(selection.empty() ? "Automatic bundled DLL" : "Selected DLL")!=std::string::npos &&
                        value.runtime.find("File found")!=std::string::npos &&
                        (pathSuffix.empty() || value.runtime.ends_with(pathSuffix)) &&
                        (expectedDescription.empty() || value.runtime==expectedDescription);
                };
                const auto start=std::chrono::steady_clock::now();
                const auto deadline=start+std::chrono::seconds(10);
                RuntimeSnapshot latest;
                bool settled=false;
                bool latestOriginal=true;
                size_t latestAlphaMismatch=0;
                auto idleAt=start;
                unsigned settlingRenders=0,confirmations=0,windowResets=0;
                do {
                    const auto pixels=captureSource(); ++settlingRenders; latest=snapshot();
                    latestOriginal=pixels==original; latestAlphaMismatch=0;
                    for (size_t i=3;i<original.size();i+=4)
                        if (pixels[i]!=original[i]) ++latestAlphaMismatch;
                    const auto now=std::chrono::steady_clock::now();
                    if (ready(latest) && !latestOriginal && latestAlphaMismatch==0) {
                        if (!confirmations) idleAt=now;
                        ++confirmations;
                        if (confirmations>=12 && now-idleAt>=std::chrono::milliseconds(300) && now<=deadline) {
                            settled=true; break;
                        }
                    } else {
                        // The selected path may update before the backend starts
                        // preparing it. Idle must remain true for the whole
                        // verification window, including resource retirement.
                        if (confirmations) ++windowResets;
                        confirmations=0; idleAt=now;
                    }
                    if (now>=deadline) break;
                    Sleep(17);
                } while (std::chrono::steady_clock::now()<deadline);
                require(settled,std::string(label)+" did not sustain selected-runtime idle processing for 300 ms and 12 renders within 10 s: "+
                        latest.status+"; "+latest.resources+"; "+latest.runtime+"; consecutiveRenders="+std::to_string(confirmations)+
                        "; pixelsOriginal="+std::to_string(latestOriginal)+"; alphaMismatch="+std::to_string(latestAlphaMismatch));
                const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
                const auto held=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-idleAt).count();
                const auto message=std::string(label)+": idle selected runtime, non-original RGB and exact alpha passed; settlingRenders="+
                    std::to_string(settlingRenders)+"; postIdleRenders="+std::to_string(confirmations)+"; postIdleMs="+
                    std::to_string(held)+"; windowResets="+std::to_string(windowResets)+"; totalMs="+std::to_string(elapsed)+"; "+latest.runtime+"\n";
                std::cout << message; runtimeSwitchMetrics+=message;
                return latest.runtime;
            };
            const auto originalRuntimeStatus=confirmRuntime("Original runtime after missing-path recovery",runtimeSelection,"");
            const auto separator=originalRuntimeStatus.find("<br>");
            require(separator!=std::string::npos,"Original runtime status did not include its resolved DLL path");
            std::string originalRuntimePath=originalRuntimeStatus.substr(separator+4);
            const auto decode=[&](const char* entity,const char* value) {
                size_t at=0;
                while ((at=originalRuntimePath.find(entity,at))!=std::string::npos) {
                    originalRuntimePath.replace(at,std::strlen(entity),value); ++at;
                }
            };
            decode("&lt;","<"); decode("&gt;",">"); decode("&amp;","&");
            require(!std::filesystem::equivalent(std::filesystem::path(std::u8string(originalRuntimePath.begin(),originalRuntimePath.end())),
                                                alternateRuntimePath),"Alternate runtime resolves to the original DLL; cross-runtime coverage requires a different file");
            p_obs_data_set_string(settings,"runtime",alternateRuntime.c_str()); p_obs_source_update(filter,settings);
            confirmRuntime("Alternate runtime",alternateRuntime,"");
            p_obs_data_set_string(settings,"runtime",runtimeSelection.c_str()); p_obs_source_update(filter,settings);
            confirmRuntime("Restored original runtime",runtimeSelection,originalRuntimeStatus);
            auto* restoredRuntime=p_obs_source_get_settings(filter);
            require(std::string(p_obs_data_get_string(restoredRuntime,"runtime"))==runtimeSelection,"Runtime switching did not restore the saved selection");
            const std::string afterRuntimeJson=p_obs_data_get_json(restoredRuntime);
            auto* actualRuntimeValues=p_obs_data_create_from_json(afterRuntimeJson.c_str());
            require(actualRuntimeValues!=nullptr,"Cannot read restored runtime-switch settings");
            const bool runtimeValuesPreserved=sameValues(sameValues,expectedRuntimeValues,actualRuntimeValues);
            if (!runtimeValuesPreserved)
                std::cerr << "Runtime settings expected JSON:\n" << beforeRuntimeJson
                          << "\nRuntime settings actual JSON:\n" << afterRuntimeJson << '\n';
            p_obs_data_release(expectedRuntimeValues); p_obs_data_release(actualRuntimeValues);
            require(runtimeValuesPreserved,"Runtime switching changed saved values or unknown fields");
            p_obs_data_release(restoredRuntime);
        }
        auto* beforeProperties=p_obs_source_get_settings(filter);
        const std::string beforePropertiesJson=p_obs_data_get_json(beforeProperties);
        p_obs_data_release(beforeProperties);
        auto* properties=p_obs_source_properties(filter);
        require(properties && p_obs_properties_get(properties,"ui_page") && p_obs_properties_get(properties,"edit_pass"),
            "Flat 5-page property layout controls missing");
        const auto visible=[&](const char* key,bool expected) {
            auto* property=p_obs_properties_get(properties,key);
            require(property && p_obs_property_visible(property)==expected,std::string("Wrong conditional property visibility: ")+key);
        };
        // Default page 0 (Quick): core controls visible; other pages hidden.
        visible("status",true); visible("ui_page",true);
        visible("view",true); visible("model_preset",true); visible("mix",true); visible("strength",true);
        visible("p1_style",true); visible("p1_intensity",true); visible("p1_tone",true); visible("p1_structure",true);
        visible("p1_skin",false); visible("p1_mask",false); visible("p1_ui",false);
        visible("passes",false); visible("edit_pass",false); visible("p2_style",false); visible("p3_style",false);
        visible("chroma",false); visible("processing_mode",false); visible("runtime",false);
        visible("diagnostic_status",expectedDebug!=0);
        require(p_obs_property_button_clicked(p_obs_properties_get(properties,"reset"),filter),"Reset button failed");

        auto* uiSettings=p_obs_data_create_from_json(beforePropertiesJson.c_str());
        // Page 1: Single-pass editor & clamping to passes count without mutating saved edit_pass.
        p_obs_data_set_int(uiSettings,"ui_page",1);
        p_obs_data_set_int(uiSettings,"passes",1);
        p_obs_data_set_int(uiSettings,"edit_pass",3);
        p_obs_properties_apply_settings(properties,uiSettings);
        visible("passes",true); visible("edit_pass",false);
        visible("p1_style",true); visible("p1_skin",true); visible("p1_mask",true); visible("p1_ui",true);
        visible("p2_style",false); visible("p3_style",false);
        p_obs_data_set_int(uiSettings,"passes",3);
        p_obs_data_set_int(uiSettings,"edit_pass",2);
        p_obs_properties_apply_settings(properties,uiSettings);
        visible("edit_pass",true);
        visible("p1_style",false); visible("p2_style",true); visible("p2_ui",true); visible("p3_style",false);

        // Page 2: Color & protection.
        p_obs_data_set_int(uiSettings,"ui_page",2);
        p_obs_properties_apply_settings(properties,uiSettings);
        visible("chroma",true); visible("midtone",true); visible("chroma_protection",true); visible("high",true);
        visible("view",false); visible("p2_style",false); visible("processing_mode",false);

        // Page 3: Performance & anti-flicker conditional controls together.
        p_obs_data_set_int(uiSettings,"ui_page",3);
        p_obs_properties_apply_settings(properties,uiSettings);
        visible("processing_mode",true);
        visible("update_interval",expectedInts[3]==1); visible("residual_transfer",expectedInts[3]==1);
        visible("residual_fallback",expectedInts[3]==1 && expectedInts[7]==0 && expectedInts[5]==1);
        visible("scaling",true);
        visible("resolution_control",expectedScaling);
        visible("percent",expectedScaling && expectedInts[2]==1);
        visible("current_percent",expectedScaling && expectedInts[2]==0);
        visible("flow",true);
        visible("amd_quality",expectedInts[7]==1); visible("nv_quality",expectedInts[7]==2);
        visible("af",true);
        visible("af_strength",expectedInts[10]!=0); visible("response_ms",expectedInts[10]!=0);
        visible("af_flow_note",expectedInts[7]==0 && expectedInts[10]>=2);

        // Page 4 & active diagnostics across pages without mutating saved JSON.
        struct DiagnosticCase { int page,debug,view; };
        for (const auto& test : {DiagnosticCase{0,0,0},{4,0,0},{0,1,0},{4,1,2},{4,1,1},{4,1,3}}) {
            p_obs_data_set_int(uiSettings,"ui_page",test.page);
            p_obs_data_set_int(uiSettings,"debug",test.debug);
            p_obs_data_set_int(uiSettings,"view",test.view);
            const std::string before=p_obs_data_get_json(uiSettings);
            p_obs_properties_apply_settings(properties,uiSettings);
            const bool correctionView=test.view==0 || test.view==2;
            visible("debug",test.page==4 && correctionView);
            visible("diagnostic_status",test.debug!=0);
            visible("performance_detail",test.page==4);
            visible("runtime",test.page==4);
            visible("runtime_status",test.page==4);
            if (test.debug) {
                const std::string message=infoText(properties,"diagnostic_status");
                require(message.find(correctionView ? "Selected correction diagnostics" : "take precedence")!=std::string::npos,
                        "Diagnostic visibility lost its active or overridden explanation");
            }
            auto* pageProperty=p_obs_properties_get(properties,"ui_page");
            require(!p_obs_property_modified(pageProperty,uiSettings),"Unchanged page requested a full form rebuild");
            p_obs_data_set_int(uiSettings,"ui_page",test.page==4 ? 0 : 4);
            require(p_obs_property_modified(pageProperty,uiSettings),"Changed page did not request a form rebuild");
            p_obs_data_set_int(uiSettings,"ui_page",test.page);
            require(p_obs_property_modified(pageProperty,uiSettings),"Restored page did not request a form rebuild");
            require(std::string(p_obs_data_get_json(uiSettings))==before,"Property visibility rewrote saved settings");
        }
        p_obs_data_set_int(uiSettings,"ui_page",4); p_obs_data_set_int(uiSettings,"view",0);
        p_obs_data_set_int(uiSettings,"debug",1); p_obs_data_set_int(uiSettings,"flow",0); p_obs_data_set_int(uiSettings,"af",2);
        p_obs_properties_apply_settings(properties,uiSettings);
        p_obs_data_set_int(uiSettings,"af",3);
        require(!p_obs_property_modified(p_obs_properties_get(properties,"af"),uiSettings),"Equivalent AF control visibility rebuilt the form");
        p_obs_data_set_int(uiSettings,"debug",2);
        require(p_obs_property_modified(p_obs_properties_get(properties,"debug"),uiSettings),"Changed diagnostic text did not request a form refresh");
        p_obs_data_release(uiSettings);
        std::cout << "Diagnostic controls: 5-page flat layout and active-state visibility preserved settings\n";
        p_obs_properties_destroy(properties);
        auto* afterProperties=p_obs_source_get_settings(filter);
        const std::string afterPropertiesJson=p_obs_data_get_json(afterProperties);
        if (afterPropertiesJson!=beforePropertiesJson)
            std::cerr << "Saved settings expected JSON:\n" << beforePropertiesJson
                      << "\nSaved settings actual JSON:\n" << afterPropertiesJson << '\n';
        require(afterPropertiesJson==beforePropertiesJson,"Properties rewrote saved or hidden settings");
        p_obs_data_release(afterProperties);
        if (record) {
            require(argc>5,"Recording needs an isolated output directory");
            const auto out=std::filesystem::absolute(argv[5]); std::filesystem::create_directories(out);
            auto* videoSettings=p_obs_data_create();
            p_obs_data_set_string(videoSettings,"rate_control","CQP");
            p_obs_data_set_int(videoSettings,"cqp",14);
            p_obs_data_set_string(videoSettings,"preset2","p4");
            auto* video=p_obs_video_encoder_create("obs_nvenc_h264_tex","test NVENC",videoSettings,nullptr);
            p_obs_data_release(videoSettings); require(video!=nullptr,"Create actual NVENC encoder");
            auto* audio=p_obs_audio_encoder_create("ffmpeg_aac","test AAC",nullptr,0,nullptr);
            require(audio!=nullptr,"Create AAC encoder");
            p_obs_encoder_set_video(video,p_obs_get_video()); p_obs_encoder_set_audio(audio,p_obs_get_audio());
            p_obs_set_output_source(0,image);
            for (int enabled=0;enabled<2;++enabled) {
                p_obs_source_set_enabled(filter,enabled!=0); Sleep(300);
                if (media) { p_obs_source_media_restart(image); p_obs_source_media_play_pause(image,false); Sleep(100); }
                auto* outputSettings=p_obs_data_create();
                const auto path=(out/(enabled ? "neural.mkv" : "original.mkv")).string();
                p_obs_data_set_string(outputSettings,"path",path.c_str());
                auto* output=p_obs_output_create("ffmpeg_muxer","isolated recording",outputSettings,nullptr);
                p_obs_data_release(outputSettings); require(output!=nullptr,"Create recording output");
                p_obs_output_set_video_encoder(output,video); p_obs_output_set_audio_encoder(output,audio,0);
                if (!p_obs_output_start(output)) {
                    const char* error=p_obs_output_get_last_error(output);
                    throw std::runtime_error(std::string("Start recording: ")+(error ? error : "no output error text"));
                }
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (std::chrono::steady_clock::now()<until) { Sleep(50); require(p_obs_output_active(output),"Recording stopped unexpectedly"); }
                p_obs_output_stop(output);
                const auto stopDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (p_obs_output_active(output) && std::chrono::steady_clock::now()<stopDeadline) Sleep(20);
                require(!p_obs_output_active(output),"Recording stop deadline exceeded");
                std::cout << "Recording " << path << "; frames=" << p_obs_output_get_total_frames(output)
                          << "; dropped=" << p_obs_output_get_frames_dropped(output) << '\n';
                p_obs_output_release(output);
                require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path)>1024,"Recording file missing or empty");
            }
            p_obs_set_output_source(0,nullptr); p_obs_encoder_release(video); p_obs_encoder_release(audio);
        }
        if (argc>5) {
            const auto out=std::filesystem::absolute(argv[5]); std::filesystem::create_directories(out);
            std::ofstream(out/"settings.json") << json;
            std::ofstream(out/"initial-settings.json") << initialJson;
            if (runtimeSwitch) std::ofstream(out/"runtime-switch.txt") << runtimeSwitchMetrics;
            if (media) std::ofstream(out/"media-motion.txt") << "originalChanges=" << movingOriginalChanges << "\nprocessedChanges=" << movingProcessedChanges
                << "\ncanvasFps=60\n" << mediaMetrics << '\n';
            std::ofstream raw(out/"original.rgba16f",std::ios::binary);
            raw.write(reinterpret_cast<const char*>(original.data()),original.size()*2); raw.close();
            std::ofstream nr(out/"processed.rgba16f",std::ios::binary);
            nr.write(reinterpret_cast<const char*>(processed.data()),processed.size()*2);
            std::ofstream(out/"dimensions.txt") << width << ' ' << height;
        }
        p_obs_source_filter_remove(image,filter); p_obs_source_release(filter); p_obs_data_release(settings);
        p_obs_source_dec_showing(image); p_obs_source_release(image);
        p_obs_enter_graphics(); p_gs_texrender_destroy(capture); p_obs_leave_graphics();
        p_obs_shutdown();
        std::cout << (identity ? "OBS identity RGB/alpha roundtrip passed\n" :
                     "OBS production module: normal NR output, alpha, exact mix-zero, multipass, JSON, missing-runtime pass-through and recovery passed\n")
                  << "changedPixels=" << changes << "; alphaMismatch=" << alphaMismatch << '\n';
        RemoveDllDirectory(cookie);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
