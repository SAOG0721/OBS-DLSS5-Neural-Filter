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
struct SurfaceInfo {
    bool available=false,texture=false;
    int x=0,y=0,width=0,height=0;
    gs_color_space source=GS_CS_SRGB,target=GS_CS_SRGB;
};
struct PropertyPhase {
    int state=-1,percent=0,model=0,mode=0,interval=0;
    Extent source{},inference{};
    bool preparing=false,hasCompleted=false,copyNotice=false;
    bool operator==(const PropertyPhase&) const = default;
};
bool updateDescription(obs_property_t* property,const std::string& description) {
    if (api.obs_property_description) {
        const auto* previous=api.obs_property_description(property);
        if (previous && description==previous) return false;
    }
    api.obs_property_set_description(property,description.c_str());
    return true;
}
void setInfoStyle(obs_property_t* property,obs_text_info_type type,bool wordWrap=true) {
    if (!property) return;
    if (api.obs_property_text_set_info_type) api.obs_property_text_set_info_type(property,type);
    if (api.obs_property_text_set_info_word_wrap) api.obs_property_text_set_info_word_wrap(property,wordWrap);
}
bool updateInfoType(obs_property_t* property,obs_text_info_type type) {
    if (!property || !api.obs_property_text_set_info_type) return false;
    if (api.obs_property_text_info_type && api.obs_property_text_info_type(property)==type) return false;
    api.obs_property_text_set_info_type(property,type);
    return true;
}
obs_property_t* addInfoRow(obs_properties_t* props,const char* key,const char* labelKey,
                           const std::string& content,obs_text_info_type type=OBS_TEXT_INFO_NORMAL) {
    auto* prop=api.obs_properties_add_text(props,key,text(labelKey),OBS_TEXT_INFO);
    api.obs_property_set_long_description(prop,content.c_str());
    setInfoStyle(prop,type,true);
    return prop;
}
bool updateInfoText(obs_property_t* property,const std::string& content) {
    if (!property) return false;
    if (api.obs_property_long_description) {
        const auto* previous=api.obs_property_long_description(property);
        if (previous && content==previous) return false;
    }
    api.obs_property_set_long_description(property,content.c_str());
    return true;
}
void help(obs_properties_t* props,const char* key,const char* label) {
    if (auto* property=api.obs_properties_get(props,key))
        api.obs_property_set_long_description(property,text(label));
}
bool isRuntimeFound(const std::string& selection,std::string* resolvedPath=nullptr,std::string* errorText=nullptr) {
    try {
        const auto path=resolveRuntimePath(selection);
        if (resolvedPath) *resolvedPath=path;
        return std::filesystem::is_regular_file(std::filesystem::path(std::u8string(path.begin(),path.end())));
    } catch (const std::exception& error) {
        if (errorText) *errorText=error.what();
        return false;
    }
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
    std::atomic<std::uint64_t> lastSettingsChange=0,copyNoticeUntilNs=0;
    SurfaceInfo surface;
    PropertyPhase propertyPhase;
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
    void report(const std::string& state,const Diagnostics& d,bool failure=false) {
        std::scoped_lock lock(diagnosticMutex);
        if (firstFailure.empty() && (failure || !d.error.empty())) firstFailure=d.firstError.empty() ? state : d.firstError;
        if (!d.error.empty() && d.error != diagnostic.error)
            api.blog(LOG_WARNING,"[DLSS5 Neural Filter] %s",d.report().c_str());
        reason=state; diagnostic=d;
    }
    void report(const std::string& state,bool failure=false) { report(state,backend->diagnostics(),failure); }
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
    s.midtone=number("midtone"); s.chromaProtection=number("chroma_protection");
    s.darkProtect=number("dark_protect"); s.brightProtect=number("bright_protect"); s.compression=number("compression");
    s.low=number("low"); s.high=number("high"); s.afStrength=number("af_strength"); s.responseMs=number("response_ms");
    s.passes=static_cast<int>(api.obs_data_get_int(data,"passes")); s.view=static_cast<int>(api.obs_data_get_int(data,"view"));
    s.debug=static_cast<int>(api.obs_data_get_int(data,"debug")); s.percent=static_cast<int>(api.obs_data_get_int(data,"percent"));
    s.flow=static_cast<int>(api.obs_data_get_int(data,"flow")); s.amdQuality=static_cast<int>(api.obs_data_get_int(data,"amd_quality"));
    s.nvQuality=static_cast<int>(api.obs_data_get_int(data,"nv_quality")); s.af=static_cast<int>(api.obs_data_get_int(data,"af"));
    s.modelPreset=static_cast<int>(api.obs_data_get_int(data,"model_preset"));
    s.resolutionControl=static_cast<int>(api.obs_data_get_int(data,"resolution_control"));
    s.processingMode=static_cast<int>(api.obs_data_get_int(data,"processing_mode"));
    s.updateInterval=static_cast<int>(api.obs_data_get_int(data,"update_interval"));
    s.residualTransfer=static_cast<int>(api.obs_data_get_int(data,"residual_transfer"));
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
    if (f.settings!=s) f.lastSettingsChange=monotonicNs();
    f.settings=std::move(s);
}
void defaults(obs_data_t* data) {
    for (const char* key : {"mix","strength","chroma","lightness","darken","brighten","midtone","low","high","af_strength"})
        api.obs_data_set_default_double(data,key,1);
    for (const char* key : {"hue","chroma_protection","dark_protect","bright_protect","compression"})
        api.obs_data_set_default_double(data,key,0);
    api.obs_data_set_default_double(data,"response_ms",80);
    api.obs_data_set_default_int(data,"schemaVersion",Settings::schemaVersion);
    api.obs_data_set_default_int(data,"ui_page",0); api.obs_data_set_default_int(data,"edit_pass",1);
    api.obs_data_set_default_int(data,"passes",1); api.obs_data_set_default_int(data,"percent",100);
    api.obs_data_set_default_int(data,"model_preset",1); api.obs_data_set_default_int(data,"resolution_control",1);
    api.obs_data_set_default_int(data,"update_interval",2);
    api.obs_data_set_default_int(data,"amd_quality",1); api.obs_data_set_default_int(data,"nv_quality",2);
    for (const char* key : {"view","debug","flow","af","processing_mode","residual_transfer"}) api.obs_data_set_default_int(data,key,0);
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
    const PropertyPhase phase{f.wasBypass ? 0 : !d.error.empty() ? 1 : d.state=="degraded" ? 2 : d.outputAvailable ? 3 : 4,
        d.currentPercent,d.modelPreset,d.processingMode,d.updateInterval,d.source,d.inference,
        d.preparingResources,d.completedFrames>0,now<f.copyNoticeUntilNs.load()};
    // Information-only reloads replace the native form. Let an active drag
    // finish, while processing/error transitions keep their existing refresh.
    const bool refreshInformation=now-f.lastSettingsChange.load()>200000000ull;
    if (api.obs_source_update_properties && f.propertyPhase!=phase && now-f.lastPropertyRefresh>500000000ull &&
        (refreshInformation || phase.state!=f.propertyPhase.state)) {
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
    bool changed=false;
    const auto show=[&](const char* key,bool visible) {
        auto* property=api.obs_properties_get(props,key);
        if (!property) return;
        if (!api.obs_property_visible || api.obs_property_visible(property)!=visible) {
            api.obs_property_set_visible(property,visible); changed=true;
        }
    };
    const auto disableItem=[&](obs_property_t* property,size_t idx,bool disabled) {
        if (!property || !api.obs_property_list_item_disable) return;
        if (!api.obs_property_list_item_disabled || api.obs_property_list_item_disabled(property,idx)!=disabled) {
            api.obs_property_list_item_disable(property,idx,disabled); changed=true;
        }
    };
    const int page=std::clamp<int>(static_cast<int>(api.obs_data_get_int(settings,"ui_page")),0,4);
    const int count=std::clamp<int>(static_cast<int>(api.obs_data_get_int(settings,"passes")),1,3);
    const int requestedPass=std::clamp<int>(static_cast<int>(api.obs_data_get_int(settings,"edit_pass")),1,3);
    const int activePass=std::min(requestedPass,count);
    if (auto* editPassProp=api.obs_properties_get(props,"edit_pass")) {
        disableItem(editPassProp,1,count<2);
        disableItem(editPassProp,2,count<3);
    }

    const auto view=api.obs_data_get_int(settings,"view");
    const bool correctionView=view==0 || view==2;
    const auto debug=std::clamp<long long>(api.obs_data_get_int(settings,"debug"),0,7);
    show("diagnostic_status",debug!=0);
    const char* debugNames[]={"DebugOff","DebugDelta","DebugBright","DebugDark","DebugProtect","DebugCompression","DebugGamut","DebugLab"};
    const auto diagnostic=correctionView ? std::string(text("ActiveDiagnostic"))+text(debugNames[debug]) : text("DebugInactiveHelp");
    changed=updateInfoText(api.obs_properties_get(props,"diagnostic_status"),escape(diagnostic)) || changed;

    // Page 0: Quick
    show("view",page==0);
    show("model_preset",page==0);
    show("mix",page==0);
    show("strength",page==0);

    // Page 1: NR passes (single-pass editor + Pass 1 core shared on Quick)
    show("passes",page==1);
    show("edit_pass",page==1 && count>1);
    for (int i=1;i<=3;++i) {
        const auto prefix="p"+std::to_string(i)+"_";
        const bool selected=(page==1 && i<=count && activePass==i);
        const bool coreVisible=selected || (page==0 && i==1);
        for (const char* suffix : {"style","intensity","tone","structure"})
            show((prefix+suffix).c_str(),coreVisible);
        for (const char* suffix : {"skin","mask","ui"})
            show((prefix+suffix).c_str(),selected);
    }

    // Page 2: Color & protection
    for (const char* key : {"chroma","lightness","midtone","darken","brighten",
                            "chroma_protection","hue","dark_protect","bright_protect","compression","low","high"})
        show(key,page==2);

    // Page 3: Performance & anti-flicker
    const bool alternating=api.obs_data_get_int(settings,"processing_mode")==1;
    const auto flow=api.obs_data_get_int(settings,"flow");
    const auto afMode=api.obs_data_get_int(settings,"af");
    const bool af=afMode!=0;
    const bool scaling=api.obs_data_get_bool(settings,"scaling");
    const bool adaptive=api.obs_data_get_int(settings,"resolution_control")==0;
    show("processing_mode",page==3);
    show("update_interval",page==3 && alternating);
    show("residual_transfer",page==3 && alternating);
    show("residual_fallback",page==3 && alternating && flow==0 && api.obs_data_get_int(settings,"residual_transfer")==1);
    show("scaling",page==3);
    show("resolution_control",page==3 && scaling);
    show("percent",page==3 && scaling && !adaptive);
    show("current_percent",page==3 && scaling && adaptive);
    show("flow",page==3);
    show("amd_quality",page==3 && flow==1);
    show("nv_quality",page==3 && flow==2);
    show("af",page==3);
    show("af_strength",page==3 && af);
    show("response_ms",page==3 && af);
    show("af_flow_note",page==3 && flow==0 && afMode>=2);

    // Page 4: Runtime & diagnostics
    show("debug",page==4 && correctionView);
    show("performance_status",page==4);
    show("performance_detail",page==4);
    show("resource_status",page==4);
    show("refresh_stats",page==4);
    show("runtime",page==4);
    show("runtime_status",page==4);
    show("reset",page==4);
    show("retry",page==4);
    show("copy",page==4);
    return changed;
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
    const auto prefix="p"+std::to_string(i)+"_";
    list(props,(prefix+"style").c_str(),"Style",{"Style0","Style1","Style2"});
    for (const auto& [key,label,helpKey] : {
        std::tuple{"intensity","Intensity","IntensityHelp"},
        std::tuple{"tone","Tone","ToneHelp"},
        std::tuple{"structure","Structure","StructureHelp"},
        std::tuple{"skin","Skin","SkinHelp"}}) {
        slider(props,(prefix+key).c_str(),label);
        help(props,(prefix+key).c_str(),helpKey);
    }
    api.obs_properties_add_bool(props,(prefix+"mask").c_str(),text("AutoMask"));
    api.obs_properties_add_bool(props,(prefix+"ui").c_str(),text("UiCorrection"));
}
bool resetButton(obs_properties_t*,obs_property_t*,void* data) {
    if (!data) return false;
    static_cast<Filter*>(data)->resetRequested=true; return true;
}
bool retryButton(obs_properties_t*,obs_property_t*,void* data) {
    if (!data) return false;
    static_cast<Filter*>(data)->retryRequested=true; return true;
}
bool refreshStatsButton(obs_properties_t*,obs_property_t*,void*) { return true; }
std::string runtimeStatus(const std::string& selection,bool* foundOut=nullptr) {
    std::string path,error;
    const bool found=isRuntimeFound(selection,&path,&error);
    if (foundOut) *foundOut=found;
    if (!error.empty()) return escape(error);
    return escape(std::string(text(selection.empty() ? "RuntimeAuto" : "RuntimeManual"))+" · "+
        text(found ? "RuntimeFound" : "RuntimeMissing"))+"<br>"+escape(path);
}
bool runtimeChanged(obs_properties_t* props,obs_property_t* prop,obs_data_t* data) {
    bool found=false;
    const auto status=runtimeStatus(api.obs_data_get_string(data,"runtime"),&found);
    auto* statusProp=api.obs_properties_get(props,"runtime_status");
    bool changed=updateInfoText(statusProp,status);
    changed=updateInfoType(statusProp,found ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_ERROR) || changed;
    changed=visibility(props,prop,data) || changed;
    return changed;
}
bool copyButton(obs_properties_t*,obs_property_t*,void* data) {
    if (!data) return false;
    auto& f=*static_cast<Filter*>(data); std::string message;
    { std::scoped_lock lock(f.diagnosticMutex);
      std::ostringstream surface;
      if (f.surface.available) surface << "viewport=" << f.surface.x << ',' << f.surface.y << ',' << f.surface.width << 'x' << f.surface.height
          << "; target=" << (f.surface.texture ? "texture" : "display") << "; sourceSpace=" << f.surface.source << "; targetSpace=" << f.surface.target;
      message="plugin=" NEURAL_PLUGIN_VERSION "\nOBS="+
        std::string(api.obs_get_version_string ? api.obs_get_version_string() : "unreported")+
        "\nstatus="+f.reason+"\nfilterFirstFailure="+f.firstFailure+"\n"+surface.str()+"\n"+f.diagnostic.report(); }
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
    f.copyNoticeUntilNs=monotonicNs()+4000000000ull;
    return true;
}
obs_properties_t* properties(void* value) {
    auto* props=api.obs_properties_create();
    std::string statusText;
    obs_text_info_type statusType=OBS_TEXT_INFO_NORMAL;
    Diagnostics snapshot;
    std::string runtimeSelection;
    bool hasSnapshot=false;
    int savedDebug=0;
    if (value) {
        auto& f=*static_cast<Filter*>(value);
        std::string reason;
        {
            std::scoped_lock lock(f.diagnosticMutex);
            snapshot=f.diagnostic; reason=f.reason; hasSnapshot=true;
        }
        {
            std::scoped_lock lock(f.settingsMutex);
            runtimeSelection=f.settings.runtime;
            savedDebug=f.settings.debug;
        }
        statusText=escape(reason);
        if (hasSnapshot && snapshot.source.width) {
            std::ostringstream summary;
            summary << " · " << snapshot.source.width << "×" << snapshot.source.height
                    << " → " << snapshot.inference.width << "×" << snapshot.inference.height;
            if (snapshot.currentPercent!=100) summary << " (" << snapshot.currentPercent << "%)";
            summary << " · " << text(snapshot.modelPreset==0 ? "PresetAutoShort" : snapshot.modelPreset==2 ? "PresetCC" : "PresetHT");
            statusText+=escape(summary.str());
        }
        if (savedDebug!=0) statusText+=" · "+escape(text("DiagnosticsActive"));
        if (monotonicNs()<f.copyNoticeUntilNs.load()) statusText+=" · "+escape(text("Copied"));
        const bool errorState=!snapshot.error.empty() || reason==text("UnsupportedGraphics") ||
            reason==text("ShaderError") || reason==text("UnsupportedSize") || reason==text("CopyFailed");
        const bool warningState=f.wasBypass || snapshot.state=="busy" || snapshot.state=="degraded" ||
            snapshot.preparingResources || reason==text("HdrBypass");
        statusType=errorState ? OBS_TEXT_INFO_ERROR : warningState ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL;
    } else statusText=escape(text("PerformanceWaiting"));
    addInfoRow(props,"status","Status",statusText,statusType);

    list(props,"ui_page","Page",{"PageQuick","PagePasses","PageColor","PagePerformance","PageDiagnostics"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"ui_page"),visibility);
    addInfoRow(props,"diagnostic_status","DiagnosticNote","",OBS_TEXT_INFO_WARNING);

    // Page 0: Quick
    list(props,"view","View",{"Normal","Original","Split","Difference"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"view"),visibility);
    list(props,"model_preset","ModelPreset",{"PresetAuto","PresetHT","PresetCC"});
    help(props,"model_preset","ModelHelp");
    slider(props,"mix","Mix",1,.01f); help(props,"mix","MixHelp");
    slider(props,"strength","Strength"); help(props,"strength","StrengthHelp");

    // Page 1: NR passes (single-pass editor + Pass 1 core controls shared on Page 0)
    list(props,"passes","PassCount",{"PassCount1","PassCount2","PassCount3"},1);
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"passes"),visibility);
    list(props,"edit_pass","EditPass",{"EditPass1","EditPass2","EditPass3"},1);
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"edit_pass"),visibility);
    for (int i=1;i<=3;++i) passProperties(props,i);

    // Page 2: Color & protection
    slider(props,"chroma","Chroma"); help(props,"chroma","ChromaHelp");
    slider(props,"lightness","Lightness"); help(props,"lightness","LightnessHelp");
    slider(props,"midtone","Midtone"); help(props,"midtone","MidtoneHelp");
    slider(props,"darken","Darken"); help(props,"darken","DarkenHelp");
    slider(props,"brighten","Brighten"); help(props,"brighten","BrightenHelp");
    slider(props,"chroma_protection","ChromaProtection",1); help(props,"chroma_protection","ChromaProtectionHelp");
    slider(props,"hue","Hue",1); help(props,"hue","HueHelp");
    slider(props,"dark_protect","DarkProtect",1); help(props,"dark_protect","DarkProtectHelp");
    slider(props,"bright_protect","BrightProtect",1); help(props,"bright_protect","BrightProtectHelp");
    slider(props,"compression","Compression",1); help(props,"compression","CompressionHelp");
    slider(props,"low","Low"); help(props,"low","LowHelp");
    slider(props,"high","High"); help(props,"high","HighHelp");

    // Page 3: Performance & anti-flicker
    list(props,"processing_mode","ProcessingMode",{"EveryFrame","Alternating"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"processing_mode"),visibility);
    help(props,"processing_mode","ProcessingHelp");
    list(props,"update_interval","UpdateInterval",{"Interval2","Interval3","Interval4"},2);
    list(props,"residual_transfer","ResidualTransfer",{"ResidualInPlace","ResidualReproject"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"residual_transfer"),visibility);
    help(props,"residual_transfer","ResidualHelp");
    addInfoRow(props,"residual_fallback","FallbackNote",escape(text("ResidualFallback")),OBS_TEXT_INFO_WARNING);
    auto* p=api.obs_properties_add_bool(props,"scaling",text("Scaling"));
    api.obs_property_set_modified_callback(p,visibility);
    list(props,"resolution_control","ResolutionControl",{"ResolutionAdaptive","ResolutionManual"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"resolution_control"),visibility);
    help(props,"resolution_control","ResolutionHelp");
    p=api.obs_properties_add_int_slider(props,"percent",text("Percent"),25,100,1);
    if (api.obs_property_int_set_suffix) api.obs_property_int_set_suffix(p,"%");
    api.obs_property_set_long_description(p,text("PercentHelp"));

    std::string metrics=escape(text("PerformanceWaiting"));
    std::string resourceState=escape(text("PerformanceWaiting"));
    std::string details=escape(text("DiagnosticSnapshotHelp"));
    std::string currentPercent=escape(text("PerformanceWaiting"));
    if (hasSnapshot && snapshot.source.width) {
        const auto& d=snapshot;
        resourceState=escape(text(d.preparingResources ? "ResourcesPreparing" : "ResourcesReady"));
        std::ostringstream s; s << d.source.width << "×" << d.source.height << " → " << d.inference.width << "×" << d.inference.height;
        metrics=s.str();
        currentPercent=std::to_string(d.currentPercent)+"%";
        if (!d.adaptiveReason.empty()) currentPercent+=" ("+escape(d.adaptiveReason)+")";
        s.str(""); s.clear();
        s << text("RequestedModel") << ' ' << text(d.modelPreset==0 ? "PresetAuto" : d.modelPreset==2 ? "PresetCC" : "PresetHT");
        s << " · " << text(d.processingMode==1 ? "Alternating" : "EveryFrame");
        if (d.processingMode==1) s << " · " << text("UpdateInterval") << ' ' << d.updateInterval;
        if (d.completedFrames) s << std::fixed << std::setprecision(2) << "<br>GPU NR " << d.gpuNrMs << " ms · D3D12 " << d.gpuTotalMs
            << " ms · " << text("OutputAge") << ' ' << d.outputAgeMs << " ms";
        s << "<br>" << text("CompletedFrames") << ' ' << d.completedFrames << " · " << text("BusyCount") << ' ' << d.busy;
        s << "<br>" << text("FullUpdates") << ' ' << d.fullUpdates << " · " << text("LightweightComposites") << ' ' << d.lightweightComposites
            << " · " << text("ResidualAge") << ' ' << d.residualAge;
        details=s.str();
    }
    addInfoRow(props,"current_percent","CurrentPercent",currentPercent);
    list(props,"flow","Flow",{"FlowNone","FlowAmd","FlowNv"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"flow"),visibility);
    help(props,"flow","FlowHelp");
    list(props,"amd_quality","AmdQuality",{"Performance","Quality"});
    list(props,"nv_quality","NvQuality",{"Performance","Balanced","Quality","HighQuality","HighestQuality"},1);
    list(props,"af","Af",{"AfOff","AfStatic","AfFlow","AfFlowPlus","AfLow"});
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"af"),visibility);
    slider(props,"af_strength","AfStrength",1); help(props,"af_strength","FractionHelp");
    p=api.obs_properties_add_float_slider(props,"response_ms",text("Response"),10,500,5);
    if (api.obs_property_float_set_suffix) api.obs_property_float_set_suffix(p," ms");
    api.obs_property_set_long_description(p,text("ResponseHelp"));
    addInfoRow(props,"af_flow_note","FlowNote",escape(text("AfFlowFallback")),OBS_TEXT_INFO_WARNING);

    // Page 4: Runtime & diagnostics
    list(props,"debug","Debug",{"DebugOff","DebugDelta","DebugBright","DebugDark","DebugProtect","DebugCompression","DebugGamut","DebugLab"});
    help(props,"debug","DebugHelp");
    api.obs_property_set_modified_callback(api.obs_properties_get(props,"debug"),visibility);
    addInfoRow(props,"performance_status","Dimensions",metrics);
    addInfoRow(props,"performance_detail","DiagnosticSnapshot",details);
    addInfoRow(props,"resource_status","ResourceStatus",resourceState,
        hasSnapshot && snapshot.preparingResources ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
    api.obs_properties_add_button2(props,"refresh_stats",text("RefreshStats"),refreshStatsButton,value);
    p=api.obs_properties_add_path(props,"runtime",text("Runtime"),OBS_PATH_FILE,"DLSSNR DLL (*.dll)",nullptr);
    api.obs_property_set_long_description(p,text("RuntimeHelp"));
    api.obs_property_set_modified_callback(p,runtimeChanged);
    bool runtimeFound=false;
    const auto runtimeInfo=runtimeStatus(runtimeSelection,&runtimeFound);
    addInfoRow(props,"runtime_status","RuntimeStatus",runtimeInfo,
        runtimeFound ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_ERROR);
    api.obs_properties_add_button2(props,"reset",text("Reset"),resetButton,value);
    api.obs_properties_add_button2(props,"retry",text("Retry"),retryButton,value);
    api.obs_properties_add_button2(props,"copy",text("Copy"),copyButton,value);
    return props;
}
void renderImpl(void* value,gs_effect_t*) {
    auto& f=*static_cast<Filter*>(value);
    Settings settings; { std::scoped_lock lock(f.settingsMutex); settings=f.settings; }
    if (f.retryRequested.exchange(false)) { f.backend->retry(); f.extent={}; f.resetRequested=true; }
    auto* target=api.obs_filter_get_target(f.source);
    auto* parent=api.obs_filter_get_parent(f.source);
    if (!target || !parent) { f.resetRequested=true; f.report("waiting for source"); api.obs_source_skip_video_filter(f.source); return; }
    settings.targetFps=api.videoTargetFps(parent);
    f.observe(parent);
    if (settings.bypass()) {
        f.wasBypass=true; f.resetRequested=true;
        const char* bypassKey=settings.view==1 ? "BypassView" : settings.mix==0 ? "BypassMix" : "BypassStrength";
        f.report(text(bypassKey));
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
        std::scoped_lock lock(f.diagnosticMutex);
        f.surface={true,api.gs_get_render_target && api.gs_get_render_target(),viewport.x,viewport.y,viewport.cx,viewport.cy,color,canvas};
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
        f.report(d.error.empty() ? state : d.error,d); api.obs_source_skip_video_filter(f.source); return;
    }
    const bool srgb=api.gs_framebuffer_srgb_enabled();
    api.gs_enable_framebuffer_srgb(true);
    api.gs_blend_state_push(); api.gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    api.gs_effect_set_texture(api.gs_effect_get_param_by_name(f.effect,"image"),f.result);
    while (api.gs_effect_loop(f.effect,"Draw")) api.gs_draw_sprite(f.result,0,size.width,size.height);
    api.gs_blend_state_pop(); api.gs_enable_framebuffer_srgb(srgb);
    const auto displayed=f.backend->diagnostics();
    f.report(text(displayed.state=="busy" ? "Busy" : "Processing"),displayed);
    if (++f.renderCount % 300 == 1) {
        const auto report=displayed.report();
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
