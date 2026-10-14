// SPDX-License-Identifier: GPL-3.0-only
#include "ObsApi.h"
#include "GpuBackend.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace neural {
namespace {
std::uint64_t monotonicNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string escape(const std::string& message) {
    std::string result;
    for (const char c : message) {
        if (c=='<') result+="&lt;"; else if (c=='>') result+="&gt;"; else if (c=='&') result+="&amp;"; else result+=c;
    }
    return result;
}
struct Filter {
    obs_source_t* source=nullptr;
    obs_weak_source_t* observed=nullptr;
    std::mutex settingsMutex,diagnosticMutex;
    Settings settings;
    Diagnostics diagnostic;
    std::string reason="waiting",firstFailure;
#ifdef NEURAL_IDENTITY_TEST
    std::unique_ptr<GpuBackend> backend=std::make_unique<GpuBackend>(BackendTestMode::Identity,3,BackendScheduling::CompletedFrames);
#else
    std::unique_ptr<GpuBackend> backend=std::make_unique<GpuBackend>(BackendTestMode::Neural,3,BackendScheduling::CompletedFrames);
#endif
    gs_texrender_t* capture=nullptr;
    gs_texture_t* result=nullptr;
    gs_effect_t* effect=nullptr;
    Extent extent;
    std::atomic<std::uint64_t> inputRevision=1,tick=1;
    std::atomic<bool> resetRequested=true,retryRequested=false;
    std::uint64_t generation=1,renderCount=0;
    std::uint64_t lastPropertyRefresh=0;
    std::string surface;
    std::string propertyPhase;
    bool wasBypass=false;
    gs_color_space lastColor=GS_CS_SRGB;
    std::int64_t mediaTime=-1;
    static void inputChanged(void* value,calldata_t*) {
        auto& f=*static_cast<Filter*>(value); ++f.inputRevision; f.resetRequested=true;
    }
    void disconnect() {
        if (!observed) return;
        if (auto* s=api.obs_weak_source_get_source(observed)) {
            auto* signals=api.obs_source_get_signal_handler(s);
            for (const char* event : {"update","media_started","media_ended","filter_add","filter_remove","reorder_filters"})
                api.signal_handler_disconnect(signals,event,inputChanged,this);
            api.obs_source_release(s);
        }
        api.obs_weak_source_release(observed); observed=nullptr;
    }
    void observe(obs_source_t* target) {
        if (observed) {
            auto* previous=api.obs_weak_source_get_source(observed);
            const bool same=previous == target;
            if (previous) api.obs_source_release(previous);
            if (same) return;
        }
        disconnect();
        if (!target) return;
        observed=api.obs_source_get_weak_source(target);
        auto* signals=api.obs_source_get_signal_handler(target);
        for (const char* event : {"update","media_started","media_ended","filter_add","filter_remove","reorder_filters"})
            api.signal_handler_connect(signals,event,inputChanged,this);
        ++inputRevision; resetRequested=true;
    }
    void report(const std::string& state,bool failure=false) {
        const auto d=backend->diagnostics();
        std::scoped_lock lock(diagnosticMutex);
        if (firstFailure.empty() && (failure || !d.error.empty())) firstFailure=d.firstError.empty() ? state : d.firstError;
        if (!d.error.empty() && d.error != diagnostic.error)
            api.blog(LOG_WARNING,"[DLSS5 Neural Filter] %s",d.report().c_str());
        reason=state; diagnostic=d;
    }
    void destroyGraphics() {
        backend.reset();
        if (result) api.gs_texture_destroy(result);
        if (capture) api.gs_texrender_destroy(capture);
        if (effect) api.gs_effect_destroy(effect);
        result=nullptr; capture=nullptr; effect=nullptr;
    }
};
void update(void* value,obs_data_t* data) {
    auto& f=*static_cast<Filter*>(value); Settings s;
    auto number=[&](const char* key) { return static_cast<float>(api.obs_data_get_double(data,key)); };
    s.mix=number("mix"); s.strength=number("strength"); s.chroma=number("chroma"); s.lightness=number("lightness");
    s.darken=number("darken"); s.brighten=number("brighten"); s.hue=number("hue");
    s.darkProtect=number("dark_protect"); s.brightProtect=number("bright_protect"); s.compression=number("compression");
    s.low=number("low"); s.high=number("high"); s.afStrength=number("af_strength"); s.responseMs=number("response_ms");
    s.passes=static_cast<int>(api.obs_data_get_int(data,"passes")); s.view=static_cast<int>(api.obs_data_get_int(data,"view"));
    s.debug=static_cast<int>(api.obs_data_get_int(data,"debug")); s.percent=static_cast<int>(api.obs_data_get_int(data,"percent"));
    s.flow=static_cast<int>(api.obs_data_get_int(data,"flow")); s.amdQuality=static_cast<int>(api.obs_data_get_int(data,"amd_quality"));
    s.nvQuality=static_cast<int>(api.obs_data_get_int(data,"nv_quality")); s.af=static_cast<int>(api.obs_data_get_int(data,"af"));
    s.advanced=api.obs_data_get_bool(data,"advanced"); s.scaling=api.obs_data_get_bool(data,"scaling");
    s.runtime=api.obs_data_get_string(data,"runtime");
    for (int i=0;i<3;++i) {
        const auto prefix="p"+std::to_string(i+1)+"_";
        auto& p=s.pass[i];
        p.style=static_cast<int>(api.obs_data_get_int(data,(prefix+"style").c_str()));
        p.intensity=number((prefix+"intensity").c_str()); p.tone=number((prefix+"tone").c_str());
        p.structure=number((prefix+"structure").c_str()); p.skin=number((prefix+"skin").c_str());
        p.mask=api.obs_data_get_bool(data,(prefix+"mask").c_str()); p.ui=api.obs_data_get_bool(data,(prefix+"ui").c_str());
    }
    s.sanitize();
    std::scoped_lock lock(f.settingsMutex);
    if (s.runtime != f.settings.runtime) f.retryRequested=true;
    f.settings=std::move(s);
}
void defaults(obs_data_t* data) {
    for (const char* key : {"mix","strength","chroma","lightness","darken","brighten","low","high","af_strength"})
        api.obs_data_set_default_double(data,key,1);
    for (const char* key : {"hue","dark_protect","bright_protect","compression"})
        api.obs_data_set_default_double(data,key,0);
    api.obs_data_set_default_double(data,"response_ms",80);
    api.obs_data_set_default_int(data,"schemaVersion",Settings::schemaVersion);
    api.obs_data_set_default_int(data,"passes",1); api.obs_data_set_default_int(data,"percent",100);
    api.obs_data_set_default_int(data,"amd_quality",1); api.obs_data_set_default_int(data,"nv_quality",2);
    for (const char* key : {"view","debug","flow","af"}) api.obs_data_set_default_int(data,key,0);
    api.obs_data_set_default_bool(data,"advanced",false); api.obs_data_set_default_bool(data,"scaling",false);
    api.obs_data_set_default_string(data,"runtime","");
    for (int i=1;i<=3;++i) {
        const auto prefix="p"+std::to_string(i)+"_";
        for (const char* key : {"intensity","tone","structure"}) api.obs_data_set_default_double(data,(prefix+key).c_str(),1);
        api.obs_data_set_default_double(data,(prefix+"skin").c_str(),0);
        api.obs_data_set_default_int(data,(prefix+"style").c_str(),0);
        api.obs_data_set_default_bool(data,(prefix+"mask").c_str(),false);
        api.obs_data_set_default_bool(data,(prefix+"ui").c_str(),false);
    }
}
void* create(obs_data_t* data,obs_source_t* source) {
    try {
        auto f=std::make_unique<Filter>(); f->source=source; update(f.get(),data);
        api.signal_handler_connect(api.obs_source_get_signal_handler(source),"enable",Filter::inputChanged,f.get());
        return f.release();
    } catch (...) {
        api.blog(LOG_WARNING,"[DLSS5 Neural Filter] Filter creation failed; allocation or configuration error");
        return nullptr;
    }
}
void destroy(void* value) {
    auto* f=static_cast<Filter*>(value); f->disconnect();
    api.signal_handler_disconnect(api.obs_source_get_signal_handler(f->source),"enable",Filter::inputChanged,f);
    api.obs_enter_graphics(); f->destroyGraphics(); GpuBackend::collectRetired(); api.obs_leave_graphics(); delete f;
}
void tick(void* value,float seconds) {
    auto& f=*static_cast<Filter*>(value); ++f.tick;
    const auto now=monotonicNs();
    const auto d=f.backend->diagnostics();
    const auto phase=f.wasBypass ? "bypass" : !d.error.empty() ? "error" : d.state=="degraded" ? "degraded" : d.outputAvailable ? "processing" : "initializing";
    if (api.obs_source_update_properties && f.propertyPhase!=phase && now-f.lastPropertyRefresh>500000000ull) {
        f.propertyPhase=phase; f.lastPropertyRefresh=now; api.obs_source_update_properties(f.source);
    }
    auto* parent=api.obs_filter_get_parent(f.source);
    if (parent && api.obs_source_media_get_time &&
        (api.obs_source_get_output_flags(parent)&OBS_SOURCE_CONTROLLABLE_MEDIA)) {
        const auto current=api.obs_source_media_get_time(parent);
        if (f.mediaTime>=0 && (current<f.mediaTime || current-f.mediaTime>std::max(250.f,seconds*4000))) {
            f.resetRequested=true; ++f.inputRevision;
        }
        f.mediaTime=current;
    }
}
void hidden(void* value) { static_cast<Filter*>(value)->resetRequested=true; }
const char* name(void*) { return "DLSS5 Neural Filter"; }
bool visibility(obs_properties_t* props,obs_property_t*,obs_data_t* settings) {
    const auto count=api.obs_data_get_int(settings,"passes");
    const bool advanced=api.obs_data_get_bool(settings,"advanced");
    api.obs_property_set_visible(api.obs_properties_get(props,"pass2"),count>=2);
    api.obs_property_set_visible(api.obs_properties_get(props,"pass3"),count>=3);
    api.obs_property_set_visible(api.obs_properties_get(props,"protection_controls"),advanced);
    api.obs_property_set_visible(api.obs_properties_get(props,"debug"),advanced);
    const auto flow=api.obs_data_get_int(settings,"flow");
    api.obs_property_set_visible(api.obs_properties_get(props,"amd_quality"),flow==1);
    api.obs_property_set_visible(api.obs_properties_get(props,"nv_quality"),flow==2);
    const bool af=api.obs_data_get_int(settings,"af")!=0;
    api.obs_property_set_visible(api.obs_properties_get(props,"af_strength"),af);
    api.obs_property_set_visible(api.obs_properties_get(props,"response_ms"),af);
    api.obs_property_set_visible(api.obs_properties_get(props,"percent"),api.obs_data_get_bool(settings,"scaling"));
    return true;
}
void slider(obs_properties_t* p,const char* key,const char* label,float max=2,float step=.05f) {
    auto* property=api.obs_properties_add_float_slider(p,key,text(label),0,max,step);
    if (max==2 && !std::string(key).ends_with("_skin") && api.obs_property_float_set_suffix)
        api.obs_property_float_set_suffix(property," ×");
}
void list(obs_properties_t* p,const char* key,const char* label,std::initializer_list<const char*> options,int first=0) {
    auto* property=api.obs_properties_add_list(p,key,text(label),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    int i=first; for (auto option : options) api.obs_property_list_add_int(property,text(option),i++);
}
void passProperties(obs_properties_t* props,int i) {
    auto* group=api.obs_properties_create(); const auto prefix="p"+std::to_string(i)+"_";
    list(group,(prefix+"style").c_str(),"Style",{"Style0","Style1","Style2"});
    api.obs_property_set_long_description(api.obs_properties_get(group,(prefix+"style").c_str()),text("StyleHelp"));
    for (const auto& [key,label] : {std::pair{"intensity","Intensity"},{"tone","Tone"},{"structure","Structure"},{"skin","Skin"}})
        slider(group,(prefix+key).c_str(),label);
    api.obs_properties_add_bool(group,(prefix+"mask").c_str(),text("AutoMask"));
    api.obs_properties_add_bool(group,(prefix+"ui").c_str(),text("UiCorrection"));
    api.obs_properties_add_group(props,("pass"+std::to_string(i)).c_str(),text(("Pass"+std::to_string(i)).c_str()),OBS_GROUP_NORMAL,group);
}
bool resetButton(obs_properties_t*,obs_property_t*,void* data) {
    if (!data) return false;
    static_cast<Filter*>(data)->resetRequested=true; return true;
}
bool retryButton(obs_properties_t*,obs_property_t*,void* data) {
    if (!data) return false;
    static_cast<Filter*>(data)->retryRequested=true; return true;
}
std::string runtimeStatus(const std::string& selection) {
    try {
        const auto path=resolveRuntimePath(selection);
        const bool found=std::filesystem::is_regular_file(std::filesystem::path(std::u8string(path.begin(),path.end())));
        return escape(std::string(text(selection.empty() ? "RuntimeAuto" : "RuntimeManual"))+" · "+
            text(found ? "RuntimeFound" : "RuntimeMissing"))+"<br>"+escape(path);
    } catch (const std::exception& error) { return escape(error.what()); }
}
bool runtimeChanged(obs_properties_t* props,obs_property_t*,obs_data_t* data) {
    const auto status=runtimeStatus(api.obs_data_get_string(data,"runtime"));
    api.obs_property_set_description(api.obs_properties_get(props,"runtime_status"),status.c_str());
    return true;
}
bool copyButton(obs_properties_t*,obs_property_t*,void* data) {
    if (!data) return false;
    auto& f=*static_cast<Filter*>(data); std::string message;
    { std::scoped_lock lock(f.diagnosticMutex); message="plugin=" NEURAL_PLUGIN_VERSION "\nOBS="+
        std::string(api.obs_get_version_string ? api.obs_get_version_string() : "unreported")+
        "\nstatus="+f.reason+"\nfilterFirstFailure="+f.firstFailure+"\n"+f.surface+"\n"+f.diagnostic.report(); }
    const auto n=MultiByteToWideChar(CP_UTF8,0,message.data(),static_cast<int>(message.size()),nullptr,0);
    if (n<=0 || !OpenClipboard(nullptr)) { f.report(text("CopyFailed"),true); return true; }
    bool copied=false;
    const auto handle=GlobalAlloc(GMEM_MOVEABLE,(n+1)*sizeof(wchar_t));
    if (handle) {
        auto* memory=static_cast<wchar_t*>(GlobalLock(handle));
        if (memory) {
            MultiByteToWideChar(CP_UTF8,0,message.data(),static_cast<int>(message.size()),memory,n); memory[n]=L'\0';
            GlobalUnlock(handle);
            if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,handle)) GlobalFree(handle);
            else copied=true;
        } else GlobalFree(handle);
    }
    CloseClipboard();
    if (!copied) { f.report(text("CopyFailed"),true); return true; }
    return false;
}
obs_properties_t* properties(void* value) {
    auto* props=api.obs_properties_create();
    std::string status;
    if (value) {
        auto& f=*static_cast<Filter*>(value);
        std::scoped_lock lock(f.diagnosticMutex); status=escape(std::string(text("Status"))+": "+f.reason);
    } else status=text("Status");
    // Keep the outer form free of labelled fields so the status uses its full
    // width. A long description on info creates another label/value pair.
    status+="<br>"+escape(text("StatusHelp"));
    api.obs_properties_add_text(props,"status",status.c_str(),OBS_TEXT_INFO);
    auto* compare=api.obs_properties_create();
    list(compare,"view","View",{"Normal","Original","Split","Difference"});
    list(compare,"debug","Debug",{"DebugOff","DebugDelta","DebugBright","DebugDark","DebugProtect","DebugCompression","DebugGamut","DebugLab"});
    api.obs_property_set_long_description(api.obs_properties_get(compare,"debug"),text("DebugHelp"));
    api.obs_properties_add_group(props,"comparison_controls",text("ComparisonControls"),OBS_GROUP_NORMAL,compare);
    auto* overall=api.obs_properties_create();
    slider(overall,"mix","Mix",1,.01f); slider(overall,"strength","Strength");
    api.obs_property_set_long_description(api.obs_properties_get(overall,"mix"),text("MixHelp"));
    api.obs_property_set_long_description(api.obs_properties_get(overall,"strength"),text("StrengthHelp"));
    api.obs_properties_add_group(props,"overall_controls",text("OverallControls"),OBS_GROUP_NORMAL,overall);
    auto* model=api.obs_properties_create();
    auto* p=api.obs_properties_add_int_slider(model,"passes",text("PassCount"),1,3,1);
    api.obs_property_set_modified_callback(p,visibility);
    api.obs_properties_add_group(props,"model_controls",text("ModelControls"),OBS_GROUP_NORMAL,model);
    for (int i=1;i<=3;++i) passProperties(props,i);
    auto* color=api.obs_properties_create();
    slider(color,"chroma","Chroma"); slider(color,"lightness","Lightness");
    slider(color,"darken","Darken"); slider(color,"brighten","Brighten");
    api.obs_properties_add_group(props,"color_controls",text("ColorControls"),OBS_GROUP_NORMAL,color);
    p=api.obs_properties_add_bool(props,"advanced",text("Advanced"));
    api.obs_property_set_modified_callback(p,visibility);
    auto* advanced=api.obs_properties_create();
    slider(advanced,"hue","Hue",1); slider(advanced,"dark_protect","DarkProtect",1);
    slider(advanced,"bright_protect","BrightProtect",1); slider(advanced,"compression","Compression",1);
    slider(advanced,"low","Low"); slider(advanced,"high","High");
    api.obs_properties_add_group(props,"protection_controls",text("ProtectionControls"),OBS_GROUP_NORMAL,advanced);
    auto* motion=api.obs_properties_create();
    list(motion,"flow","Flow",{"FlowNone","FlowAmd","FlowNv"});
    api.obs_property_set_modified_callback(api.obs_properties_get(motion,"flow"),visibility);
    list(motion,"amd_quality","AmdQuality",{"Performance","Quality"});
    list(motion,"nv_quality","NvQuality",{"Performance","Balanced","Quality","HighQuality","HighestQuality"},1);
    list(motion,"af","Af",{"AfOff","AfStatic","AfFlow","AfFlowPlus","AfLow"});
    api.obs_property_set_modified_callback(api.obs_properties_get(motion,"af"),visibility);
    api.obs_property_set_long_description(api.obs_properties_get(motion,"flow"),text("FlowHelp"));
    slider(motion,"af_strength","AfStrength",1);
    api.obs_property_set_long_description(api.obs_properties_get(motion,"af_strength"),text("FractionHelp"));
    api.obs_properties_add_float_slider(motion,"response_ms",text("Response"),10,500,5);
    api.obs_properties_add_group(props,"motion_controls",text("MotionControls"),OBS_GROUP_NORMAL,motion);
    auto* performance=api.obs_properties_create();
    p=api.obs_properties_add_bool(performance,"scaling",text("Scaling"));
    api.obs_property_set_modified_callback(p,visibility);
    api.obs_properties_add_int_slider(performance,"percent",text("Percent"),25,100,1);
    std::string metrics=text("PerformanceWaiting");
    if (value) {
        auto& f=*static_cast<Filter*>(value); Diagnostics d;
        { std::scoped_lock lock(f.diagnosticMutex); d=f.diagnostic; }
        if (d.source.width) {
            std::ostringstream s; s << d.source.width << "×" << d.source.height << " → " << d.inference.width << "×" << d.inference.height;
            if (d.completedFrames) s << std::fixed << std::setprecision(2) << "<br>GPU NR " << d.gpuNrMs << " ms · D3D12 " << d.gpuTotalMs
                << " ms · " << text("OutputAge") << ' ' << d.outputAgeMs << " ms";
            s << "<br>" << text("CompletedFrames") << ' ' << d.completedFrames << " · " << text("BusyCount") << ' ' << d.busy;
            metrics=s.str();
        }
    }
    api.obs_properties_add_text(performance,"performance_status",metrics.c_str(),OBS_TEXT_INFO);
    api.obs_properties_add_group(props,"performance_controls",text("PerformanceControls"),OBS_GROUP_NORMAL,performance);
    auto* runtimeControls=api.obs_properties_create();
    p=api.obs_properties_add_path(runtimeControls,"runtime",text("Runtime"),OBS_PATH_FILE,"DLSSNR DLL (*.dll)",nullptr);
    api.obs_property_set_long_description(p,text("RuntimeHelp"));
    api.obs_property_set_modified_callback(p,runtimeChanged);
    std::string selection;
    if (value) { auto& f=*static_cast<Filter*>(value); std::scoped_lock lock(f.settingsMutex); selection=f.settings.runtime; }
    const auto runtimeInfo=runtimeStatus(selection);
    api.obs_properties_add_text(runtimeControls,"runtime_status",runtimeInfo.c_str(),OBS_TEXT_INFO);
    api.obs_properties_add_button2(runtimeControls,"reset",text("Reset"),resetButton,value);
    api.obs_properties_add_button2(runtimeControls,"retry",text("Retry"),retryButton,value);
    api.obs_properties_add_button2(runtimeControls,"copy",text("Copy"),copyButton,value);
    api.obs_properties_add_group(props,"runtime_controls",text("RuntimeControls"),OBS_GROUP_NORMAL,runtimeControls);
    return props;
}
void renderImpl(void* value,gs_effect_t*) {
    auto& f=*static_cast<Filter*>(value);
    Settings settings; { std::scoped_lock lock(f.settingsMutex); settings=f.settings; }
    if (f.retryRequested.exchange(false)) { f.backend->retry(); f.extent={}; f.resetRequested=true; }
    auto* target=api.obs_filter_get_target(f.source);
    auto* parent=api.obs_filter_get_parent(f.source);
    if (!target || !parent) { f.resetRequested=true; f.report("waiting for source"); api.obs_source_skip_video_filter(f.source); return; }
    f.observe(parent);
    if (settings.bypass()) {
        f.wasBypass=true; f.resetRequested=true; f.report(text("Bypass"));
        api.obs_source_skip_video_filter(f.source); return;
    }
    if (f.wasBypass) { f.resetRequested=true; f.wasBypass=false; }
    if (api.gs_get_device_type()!=GS_DEVICE_DIRECT3D_11 || !api.obs_source_get_color_space) {
        f.report(text("UnsupportedGraphics")); api.obs_source_skip_video_filter(f.source); return;
    }
    const gs_color_space spaces[]{GS_CS_SRGB,GS_CS_SRGB_16F};
    const auto color=api.obs_source_get_color_space(target,2,spaces);
    const auto canvas=api.gs_get_color_space();
    if (api.gs_get_viewport) {
        gs_rect viewport{}; api.gs_get_viewport(&viewport);
        std::ostringstream s; s << "viewport=" << viewport.x << ',' << viewport.y << ',' << viewport.cx << 'x' << viewport.cy
            << "; target=" << (api.gs_get_render_target && api.gs_get_render_target() ? "texture" : "display")
            << "; sourceSpace=" << color << "; targetSpace=" << canvas;
        std::scoped_lock lock(f.diagnosticMutex); f.surface=s.str();
    }
    if ((color!=GS_CS_SRGB && color!=GS_CS_SRGB_16F) || (canvas!=GS_CS_SRGB && canvas!=GS_CS_SRGB_16F)) {
        f.resetRequested=true; f.report(text("HdrBypass")); api.obs_source_skip_video_filter(f.source); return;
    }
    if (color != f.lastColor) { f.lastColor=color; ++f.generation; f.resetRequested=true; }
    Extent size{api.obs_source_get_width(target),api.obs_source_get_height(target)};
    if (!size.width || !size.height) { f.resetRequested=true; f.report("waiting for pixels"); api.obs_source_skip_video_filter(f.source); return; }
    if (size.width>3840 || size.height>2160 || !safeExtent(inferenceExtent(size,settings))) {
        f.resetRequested=true; f.report(text("UnsupportedSize")); api.obs_source_skip_video_filter(f.source); return;
    }
    if (size != f.extent) {
        if (f.result) api.gs_texture_destroy(f.result);
        f.result=api.gs_texture_create(size.width,size.height,GS_RGBA16F,1,nullptr,0);
        if (!f.capture) f.capture=api.gs_texrender_create(GS_RGBA16F,GS_ZS_NONE);
        f.extent=size; ++f.generation; f.resetRequested=true;
    }
    if (!f.effect) {
        const auto path=dataDirectory()+"/output.effect";
        f.effect=api.gs_effect_create_from_file(path.c_str(),nullptr);
    }
    if (!f.result || !f.capture) {
        f.report("GPU capture/output texture allocation failed; retry initialization",true);
        api.obs_source_skip_video_filter(f.source); return;
    }
    if (!f.effect) {
        f.report(text("ShaderError"),true); api.obs_source_skip_video_filter(f.source); return;
    }
    api.gs_texrender_reset(f.capture);
    if (!api.gs_texrender_begin_with_color_space(f.capture,size.width,size.height,GS_CS_SRGB_16F)) {
        f.report("input capture failed",true); api.obs_source_skip_video_filter(f.source); return;
    }
    api.gs_blend_state_push();
    api.gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    vec4 clear{}; api.gs_clear(GS_CLEAR_COLOR,&clear,0,0);
    api.gs_ortho(0,static_cast<float>(size.width),0,static_cast<float>(size.height),-100,100);
    const auto flags=api.obs_source_get_output_flags(parent);
    if (target==parent && !(flags&(OBS_SOURCE_CUSTOM_DRAW|OBS_SOURCE_ASYNC))) api.obs_source_default_render(target);
    else api.obs_source_video_render(target);
    api.gs_blend_state_pop(); api.gs_texrender_end(f.capture);
    auto* texture=api.gs_texrender_get_texture(f.capture);
    auto* input=static_cast<ID3D11Texture2D*>(api.gs_texture_get_obj(texture));
    auto* output=static_cast<ID3D11Texture2D*>(api.gs_texture_get_obj(f.result));
    auto* device=static_cast<ID3D11Device*>(api.gs_get_device_obj());
    const auto clock=api.obs_get_video_frame_time ? api.obs_get_video_frame_time() : monotonicNs();
    const auto cycle=api.obs_get_video_frame_time ? clock : f.tick.load();
    FrameKey key{cycle,f.inputRevision.load(),f.generation,size};
    const bool processed=f.backend->process(device,input,output,settings,key,clock,f.resetRequested.exchange(false));
    if (!processed) {
        const auto d=f.backend->diagnostics();
        const auto state=d.state=="busy" ? text("Busy") : d.state=="degraded" ? text("Degraded") : text("Initializing");
        f.report(d.error.empty() ? state : d.error); api.obs_source_skip_video_filter(f.source); return;
    }
    const bool srgb=api.gs_framebuffer_srgb_enabled();
    api.gs_enable_framebuffer_srgb(true);
    api.gs_blend_state_push(); api.gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    api.gs_effect_set_texture(api.gs_effect_get_param_by_name(f.effect,"image"),f.result);
    while (api.gs_effect_loop(f.effect,"Draw")) api.gs_draw_sprite(f.result,0,size.width,size.height);
    api.gs_blend_state_pop(); api.gs_enable_framebuffer_srgb(srgb);
    const auto displayed=f.backend->diagnostics();
    f.report(text(displayed.state=="busy" ? "Busy" : "Processing"));
    if (++f.renderCount % 300 == 1) {
        const auto report=f.backend->diagnostics().report();
        api.blog(LOG_INFO,"[DLSS5 Neural Filter] %s",report.c_str());
    }
}
void render(void* value,gs_effect_t* effect) {
    try { renderImpl(value,effect); }
    catch (const std::exception& e) {
        try { static_cast<Filter*>(value)->report(e.what(),true); } catch (...) {}
        api.obs_source_skip_video_filter(static_cast<Filter*>(value)->source);
    } catch (...) {
        api.obs_source_skip_video_filter(static_cast<Filter*>(value)->source);
    }
}
gs_color_space colorSpace(void* value,size_t count,const gs_color_space* preferred) {
    auto& f=*static_cast<Filter*>(value); auto* target=api.obs_filter_get_target(f.source);
    if (target && api.obs_source_get_color_space) return api.obs_source_get_color_space(target,count,preferred);
    return GS_CS_SRGB;
}
}
void registerFilter() {
    obs_source_info source{};
    source.id="dlss5_neural_filter"; source.type=OBS_SOURCE_TYPE_FILTER;
    source.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW|OBS_SOURCE_SRGB;
    source.get_name=name; source.create=create; source.destroy=destroy; source.update=update;
    source.get_defaults=defaults; source.get_properties=properties; source.video_tick=tick; source.video_render=render;
    source.show=hidden; source.hide=hidden; source.video_get_color_space=colorSpace;
    // libobs rejects a source definition larger than its own struct. Register
    // only the public prefix actually used, without looking at the host version.
    const auto bytes=api.obs_source_get_color_space ?
        offsetof(obs_source_info,video_get_color_space)+sizeof(source.video_get_color_space) :
        offsetof(obs_source_info,video_render)+sizeof(source.video_render);
    api.obs_register_source_s(&source,bytes);
}
}
