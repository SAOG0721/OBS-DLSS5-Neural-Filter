// SPDX-License-Identifier: GPL-3.0-only
// Actual libobs + image-source + production plugin; isolated from user settings.
#include "ObsApi.h"
#include <Windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        require(argc>=5,"Usage: obs_harness OBS-runtime-root plugin-path image-path runtime-DLL [output-dir]");
        const bool identity=argc>6 && std::string(argv[6])=="--identity";
        const bool monitor=argc>6 && std::string(argv[6])=="--monitor";
        const bool dual=argc>6 && std::string(argv[6])=="--dual-preview";
        const bool steady=argc>6 && std::string(argv[6])=="--steady-1440p";
        const bool recovery=argc>6 && std::string(argv[6])=="--record-recovery";
        const bool record=argc>6 && (std::string(argv[6])=="--record" || recovery);
        const std::string runtimeSelection=std::string(argv[4])=="--private-runtime" ? "" :
            std::filesystem::absolute(argv[4]).string();
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
        B(obs_source_filter_set_order)
        B(obs_source_filter_add) B(obs_source_filter_remove) B(obs_source_properties) B(obs_source_get_settings)
        B(obs_source_get_width) B(obs_source_get_height) B(obs_source_video_render) B(obs_source_inc_showing) B(obs_source_dec_showing)
        B(obs_data_create) B(obs_data_release) B(obs_data_set_string) B(obs_data_set_double) B(obs_data_set_int)
        B(obs_data_get_int) B(obs_data_get_json) B(obs_data_create_from_json)
        B(obs_properties_destroy) B(obs_properties_get) B(obs_property_button_clicked)
        B(obs_property_description)
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
        if (record) {
            obs_audio_info audio{}; audio.samples_per_sec=48000; audio.speakers=SPEAKERS_STEREO;
            require(p_obs_reset_audio(&audio),"OBS audio initialization failed");
            module(root/"obs-plugins"/"64bit"/"obs-ffmpeg.dll",root/"data"/"obs-plugins"/"obs-ffmpeg");
            module(root/"obs-plugins"/"64bit"/"obs-nvenc.dll",root/"data"/"obs-plugins"/"obs-nvenc");
        }
        const auto pluginData=argc>7 ? std::filesystem::absolute(argv[7]) :
            std::filesystem::absolute(argv[2]).parent_path().parent_path().parent_path()/"data";
        module(std::filesystem::absolute(argv[2]),pluginData);
        auto* imageSettings=p_obs_data_create();
        p_obs_data_set_string(imageSettings,monitor ? "monitor_id" : "file",monitor ? argv[3] : std::filesystem::absolute(argv[3]).string().c_str());
        auto* image=p_obs_source_create(monitor ? "monitor_capture" : "image_source","fixture",imageSettings,nullptr);
        p_obs_data_release(imageSettings); require(image!=nullptr,"Create OBS image source"); p_obs_source_inc_showing(image);
        if (monitor) p_obs_set_output_source(0,image);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (!p_obs_source_get_width(image) && std::chrono::steady_clock::now()<deadline) Sleep(10);
        const unsigned width=p_obs_source_get_width(image),height=p_obs_source_get_height(image);
        require(width && height,"Image source has no pixels");
        auto* settings=p_obs_data_create(); p_obs_data_set_string(settings,"runtime",runtimeSelection.c_str());
        auto* filter=p_obs_source_create("dlss5_neural_filter","DLSS5 Neural Filter",settings,nullptr);
        require(filter!=nullptr,"Create production source filter"); p_obs_source_filter_add(image,filter);
        if (monitor || steady) {
            p_obs_set_output_source(0,image);
            char duration[32]{}; const auto hasDuration=GetEnvironmentVariableA("NEURAL_HARNESS_SECONDS",duration,sizeof(duration));
            const int seconds=hasDuration ? std::clamp(std::stoi(duration),1,14400) : 30;
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
            std::string status;
            while (std::chrono::steady_clock::now()<end) {
                Sleep(500);
                auto* props=p_obs_source_properties(filter);
                status=p_obs_property_description(p_obs_properties_get(props,"status"));
                const auto runtime=p_obs_property_description(p_obs_properties_get(props,"runtime_status"));
                require(runtime && std::string(runtime).find("File found")!=std::string::npos,"Bundled runtime status did not find DLL");
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
        p_obs_source_set_enabled(filter,false); Sleep(50);
        const auto original=captureSource();
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
        require(captureSource()==original,"OBS mix-zero did not restore original exactly");
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
        p_obs_data_release(persisted); p_obs_data_release(restored);
        auto* duplicate=p_obs_source_duplicate(image,"independent fixture",false);
        require(duplicate!=nullptr,"Source duplicate failed");
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
        auto* properties=p_obs_source_properties(filter);
        require(properties && p_obs_properties_get(properties,"pass3"),"Native property groups missing");
        require(p_obs_property_button_clicked(p_obs_properties_get(properties,"reset"),filter),"Reset button failed");
        p_obs_properties_destroy(properties);
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
