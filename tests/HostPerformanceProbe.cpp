// SPDX-License-Identifier: GPL-3.0-only
// Actual isolated libobs. Timed loops contain no Qt or GPU readback.
#include "ObsApi.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
std::uint64_t monotonicNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
struct HostSignals {
    std::atomic<std::uint64_t> acceptedUpdates=0,acceptedTimeNs=0,propertyRefreshes=0;
    static void accepted(void* data,calldata_t*) {
        auto& signals=*static_cast<HostSignals*>(data);
        signals.acceptedTimeNs=monotonicNs(); ++signals.acceptedUpdates;
    }
    static void properties(void* data,calldata_t*) { ++static_cast<HostSignals*>(data)->propertyRefreshes; }
};
void require(bool condition,const std::string& reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T bind(HMODULE module,const char* name) {
    auto* value=GetProcAddress(module,name);
    require(value!=nullptr,std::string("Missing probe capability: ")+name);
    return reinterpret_cast<T>(value);
}
std::string quoted(const std::string& value) {
    std::string result="\"";
    for (unsigned char c : value) {
        if (c=='\\' || c=='"') { result+='\\'; result+=static_cast<char>(c); }
        else if (c=='\n') result+="\\n";
        else if (c=='\r') result+="\\r";
        else if (c=='\t') result+="\\t";
        else if (c>=32) result+=static_cast<char>(c);
    }
    return result+'"';
}
struct Distribution {
    std::vector<double> values;
    void add(double value) { values.push_back(value); }
    std::string json() const {
        if (values.empty()) return "null";
        auto sorted=values; std::sort(sorted.begin(),sorted.end());
        const auto percentile=[&](double p) { return sorted[static_cast<size_t>(std::ceil(p*static_cast<double>(sorted.size())))-1]; };
        std::ostringstream out; out << std::fixed << std::setprecision(6)
            << "{\"count\":" << sorted.size() << ",\"p50Ms\":" << percentile(.5)
            << ",\"p95Ms\":" << percentile(.95) << ",\"maxMs\":" << sorted.back() << '}';
        return out.str();
    }
};
struct Snapshot {
    std::string status,detail;
    std::uint64_t completed=0;
    double propertiesMs=0;
    bool counterKnown=false;
    bool backgroundKnown=false,backgroundBusy=false;
    bool available() const { return status.find("Processing")!=std::string::npos || status.find("completed result")!=std::string::npos; }
};
struct Frame { double updateMs=-1,renderMs=0,graphicsMs=0,lockMs=0,propertiesMs=0; };
struct Phase {
    std::string name;
    Distribution update,render,graphics,lock,propertyPoll;
    Snapshot before,after;
    double seconds=0,firstAvailableMs=-1,firstAcceptedMs=-1,longestUnavailableMs=0;
    std::uint64_t requestedUpdates=0,acceptedUpdates=0,propertyRefreshes=0;
    int availableCheckedFrames=0,unavailableObservations=0,acceptedAvailabilityChecks=0;
    std::string json() const {
        std::ostringstream out; out << std::fixed << std::setprecision(6)
            << "{\"name\":" << quoted(name) << ",\"seconds\":" << seconds
            << ",\"sourceUpdate\":" << update.json() << ",\"sourceVideoRender\":" << render.json()
            << ",\"graphicsScope\":" << graphics.json() << ",\"graphicsLock\":" << lock.json()
            << ",\"availabilityPropertyPoll\":" << propertyPoll.json()
            << ",\"firstAvailableObservationMs\":" << (firstAvailableMs<0 ? "null" : std::to_string(firstAvailableMs))
            << ",\"firstAcceptedUpdateMs\":" << (firstAcceptedMs<0 ? "null" : std::to_string(firstAcceptedMs))
            << ",\"firstAvailableAfterAcceptedMs\":" << (firstAvailableMs<0 || firstAcceptedMs<0 ? "null" : std::to_string(std::max(0.0,firstAvailableMs-firstAcceptedMs)))
            << ",\"availabilityCheckedFrames\":" << availableCheckedFrames
            << ",\"availabilityChecksAfterAcceptance\":" << acceptedAvailabilityChecks
            << ",\"unavailableObservationsAfterAcceptance\":" << unavailableObservations
            << ",\"longestObservedUnavailableMs\":" << longestUnavailableMs
            << ",\"requestedUpdates\":" << requestedUpdates << ",\"acceptedUpdates\":" << acceptedUpdates
            << ",\"propertyRefreshRequests\":" << propertyRefreshes;
        if (before.counterKnown && after.counterKnown && after.completed>=before.completed) {
            out << ",\"completedFrames\":" << after.completed-before.completed
                << ",\"completedFramesPerSecond\":" << static_cast<double>(after.completed-before.completed)/seconds;
        } else out << ",\"completedFrames\":null,\"completedFramesPerSecond\":null";
        out << ",\"backgroundStateKnown\":" << (after.backgroundKnown ? "true" : "false")
            << ",\"backgroundWorkPendingAfterPhase\":" << (after.backgroundBusy ? "true" : "false")
            << ",\"beforeStatus\":" << quoted(before.status) << ",\"afterStatus\":" << quoted(after.status)
            << ",\"beforeDiagnostics\":" << quoted(before.detail) << ",\"afterDiagnostics\":" << quoted(after.detail) << '}';
        return out.str();
    }
};

int main(int argc,char** argv) {
    try {
        require(argc>=7,"Usage: host_performance_probe OBS-root plugin image runtime-DLL output-dir plugin-data [frames=120] [percent-step=5]");
        const auto root=std::filesystem::absolute(argv[1]);
        const auto plugin=std::filesystem::absolute(argv[2]);
        const auto imagePath=std::filesystem::absolute(argv[3]);
        const std::string runtime=std::string(argv[4])=="--private-runtime" ? "" : std::filesystem::absolute(argv[4]).string();
        const auto output=std::filesystem::absolute(argv[5]);
        const auto pluginData=std::filesystem::absolute(argv[6]);
        const int frames=argc>7 ? std::clamp(std::stoi(argv[7]),30,600) : 120;
        const int percentStep=argc>8 ? std::clamp(std::stoi(argv[8]),1,25) : 5;
        std::filesystem::create_directories(output);
        const auto binary=root/"bin"/"64bit";
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const auto cookie=AddDllDirectory(binary.c_str()); require(cookie!=nullptr,"Add isolated OBS directory");
        const auto dll=LoadLibraryExW((binary/"obs.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(dll!=nullptr,"Load isolated OBS library");
#define B(name) auto p_##name=bind<decltype(&::name)>(dll,#name);
        B(obs_startup) B(obs_shutdown) B(obs_add_data_path) B(obs_reset_video) B(obs_open_module) B(obs_init_module)
        B(obs_source_create) B(obs_source_release) B(obs_source_update) B(obs_source_inc_showing) B(obs_source_dec_showing)
        B(obs_source_filter_add) B(obs_source_filter_remove) B(obs_source_video_render) B(obs_source_get_width) B(obs_source_get_height)
        B(obs_source_properties) B(obs_source_get_settings)
        B(obs_source_get_signal_handler) B(signal_handler_connect) B(signal_handler_disconnect)
        B(obs_data_create) B(obs_data_release) B(obs_data_set_string) B(obs_data_set_double) B(obs_data_set_int) B(obs_data_set_bool)
        B(obs_data_apply) B(obs_data_clear) B(obs_data_create_from_json) B(obs_data_get_json) B(obs_data_get_json_with_defaults)
        B(obs_properties_destroy) B(obs_properties_get) B(obs_properties_apply_settings) B(obs_property_modified) B(obs_property_description) B(obs_property_long_description)
        B(obs_enter_graphics) B(obs_leave_graphics) B(gs_texrender_create) B(gs_texrender_destroy) B(gs_texrender_reset)
        B(gs_texrender_begin_with_color_space) B(gs_texrender_end) B(gs_clear) B(gs_ortho)
        B(gs_blend_state_push) B(gs_blend_state_pop) B(gs_blend_function)
#undef B
        require(p_obs_startup("en-US",nullptr,nullptr),"OBS startup failed");
        p_obs_add_data_path(((root/"data"/"libobs").string()+"/").c_str());
        obs_video_info video{}; const auto graphicsModule=(binary/"libobs-d3d11.dll").string();
        video.graphics_module=graphicsModule.c_str(); video.base_width=1280; video.base_height=720;
        video.output_width=1280; video.output_height=720; video.fps_num=60; video.fps_den=1;
        video.output_format=VIDEO_FORMAT_RGBA; video.colorspace=VIDEO_CS_709; video.range=VIDEO_RANGE_FULL;
        require(p_obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"OBS video initialization failed");
        const auto load=[&](const std::filesystem::path& file,const std::filesystem::path& data) {
            obs_module_t* module=nullptr;
            require(p_obs_open_module(&module,file.string().c_str(),data.string().c_str())==MODULE_SUCCESS,"Open OBS module: "+file.string());
            require(p_obs_init_module(module),"Initialize OBS module: "+file.string());
        };
        load(root/"obs-plugins"/"64bit"/"image-source.dll",root/"data"/"obs-plugins"/"image-source");
        load(plugin,pluginData);
        auto* sourceSettings=p_obs_data_create(); p_obs_data_set_string(sourceSettings,"file",imagePath.string().c_str());
        auto* source=p_obs_source_create("image_source","performance fixture",sourceSettings,nullptr);
        p_obs_data_release(sourceSettings); require(source!=nullptr,"Create image source"); p_obs_source_inc_showing(source);
        const auto sizeDeadline=Clock::now()+std::chrono::seconds(3);
        while (!p_obs_source_get_width(source) && Clock::now()<sizeDeadline) Sleep(10);
        const unsigned width=p_obs_source_get_width(source),height=p_obs_source_get_height(source);
        require(width>=512 && height>=288,"Fixture must remain safe at 50% inference resolution");
        auto* settings=p_obs_data_create(); p_obs_data_set_string(settings,"runtime",runtime.c_str());
        auto* filter=p_obs_source_create("dlss5_neural_filter","performance filter",settings,nullptr);
        require(filter!=nullptr,"Create production filter"); p_obs_source_filter_add(source,filter);
        HostSignals signals; auto* sourceSignals=p_obs_source_get_signal_handler(filter);
        p_signal_handler_connect(sourceSignals,"update",HostSignals::accepted,&signals);
        p_signal_handler_connect(sourceSignals,"update_properties",HostSignals::properties,&signals);
        auto* effective=p_obs_source_get_settings(filter);
        const std::string defaultsJson=p_obs_data_get_json_with_defaults(effective); p_obs_data_release(effective);
        auto* defaults=p_obs_data_create_from_json(defaultsJson.c_str()); require(defaults!=nullptr,"Clone effective settings");
        p_obs_enter_graphics(); auto* capture=p_gs_texrender_create(GS_RGBA16F,GS_ZS_NONE); p_obs_leave_graphics();
        require(capture!=nullptr,"Create probe capture target");
        const auto render=[&]() {
            Frame frame; const auto scopeStart=Clock::now(); p_obs_enter_graphics(); frame.lockMs=elapsed(scopeStart);
            p_gs_texrender_reset(capture);
            require(p_gs_texrender_begin_with_color_space(capture,width,height,GS_CS_SRGB_16F),"Begin probe render");
            vec4 clear{}; p_gs_clear(GS_CLEAR_COLOR,&clear,0,0);
            p_gs_ortho(0,static_cast<float>(width),0,static_cast<float>(height),-100,100);
            p_gs_blend_state_push(); p_gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
            const auto start=Clock::now(); p_obs_source_video_render(source); frame.renderMs=elapsed(start);
            p_gs_blend_state_pop(); p_gs_texrender_end(capture); p_obs_leave_graphics();
            frame.graphicsMs=elapsed(scopeStart); return frame;
        };
        const auto infoText=[&](obs_properties_t* props,const char* key) {
            auto* property=p_obs_properties_get(props,key);
            if (!property) return std::string();
            const auto* detail=p_obs_property_long_description(property);
            if (detail && *detail) return std::string(detail);
            const auto* desc=p_obs_property_description(property);
            return desc ? std::string(desc) : std::string();
        };
        const auto snapshot=[&]() {
            Snapshot result; const auto start=Clock::now(); auto* props=p_obs_source_properties(filter);
            require(props!=nullptr,"Read filter properties");
            result.status=infoText(props,"status");
            for (const char* key : {"performance_status","performance_detail"}) {
                const auto value=infoText(props,key);
                if (!value.empty()) result.detail+=value+"\n";
            }
            const auto resource=infoText(props,"resource_status");
            if (!resource.empty()) {
                result.backgroundKnown=true;
                result.backgroundBusy=resource.find("Preparing or retiring")!=std::string::npos;
                result.detail+=resource+'\n';
            }
            p_obs_properties_destroy(props); result.propertiesMs=elapsed(start);
            const auto position=result.detail.find("Completed ");
            if (position!=std::string::npos) {
                result.completed=std::stoull(result.detail.substr(position+10)); result.counterKnown=true;
            }
            return result;
        };
        const auto warm=[&](int minimum=12) {
            Snapshot state; const auto deadline=Clock::now()+std::chrono::seconds(8);
            int count=0;
            do { render(); ++count; if (count>=minimum) state=snapshot(); Sleep(17); }
            while ((count<minimum || !state.available() || state.backgroundBusy) && Clock::now()<deadline);
            require(state.available() && !state.backgroundBusy,"Warm-up did not settle with an available neural result: "+state.status);
        };
        const auto restore=[&](int passes=1) {
            p_obs_data_clear(settings); p_obs_data_apply(settings,defaults); p_obs_data_set_int(settings,"passes",passes);
            p_obs_source_update(filter,settings); warm();
        };
        std::vector<Phase> phases;
        std::ofstream samples(output/"samples.csv");
        samples << "phase,index,update_ms,source_render_ms,graphics_scope_ms,graphics_lock_ms,availability_properties_ms,accepted_updates,property_refresh_requests,availability_known,available,after_accepted_update\n";
        const auto save=[&]() {
            std::ofstream report(output/"performance.json");
            report << "{\"scope\":\"Actual isolated libobs, no Qt; timed loops contain no GPU readback. Full recovery windows are observed after the host update signal; available output may be a retained completed result and is not proof of a particular parameter revision.\","
                << "\"probeSchemaVersion\":3,\"percentileMethod\":\"nearest rank\",\"plugin\":" << quoted(plugin.string())
                << ",\"runtime\":" << quoted(runtime) << ",\"width\":" << width << ",\"height\":" << height
                << ",\"targetFps\":60,\"framesPerGradient\":" << frames << ",\"percentStep\":" << percentStep << ",\"phases\":[";
            for (size_t i=0;i<phases.size();++i) { if (i) report << ','; report << phases[i].json(); }
            report << "]}\n";
        };
        const auto phase=[&](const std::string& name,int count,const std::function<bool(int)>& change={},bool recovery=false) {
            Phase result; result.name=name; result.before=snapshot();
            const auto acceptedBefore=signals.acceptedUpdates.load();
            const auto refreshesBefore=signals.propertyRefreshes.load();
            const auto started=Clock::now(); auto due=started;
            const auto startedNs=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count());
            std::uint64_t firstRequestNs=0;
            double unavailableSince=-1;
            for (int i=0;i<count;++i) {
                std::this_thread::sleep_until(due); due+=std::chrono::microseconds(16667);
                double updateMs=-1;
                if (change && change(i)) {
                    const auto updateStart=Clock::now();
                    if (!firstRequestNs) firstRequestNs=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(updateStart.time_since_epoch()).count());
                    p_obs_source_update(filter,settings); updateMs=elapsed(updateStart); result.update.add(updateMs);
                    ++result.requestedUpdates;
                }
                auto frame=render(); frame.updateMs=updateMs;
                const auto accepted=signals.acceptedUpdates.load()-acceptedBefore;
                const auto acceptedAt=signals.acceptedTimeNs.load();
                const bool afterAccepted=accepted!=0 && firstRequestNs && acceptedAt>=firstRequestNs;
                if (afterAccepted && result.firstAcceptedMs<0)
                    result.firstAcceptedMs=static_cast<double>(acceptedAt-startedNs)/1000000.0;
                bool available=false;
                if (recovery) {
                    const auto state=snapshot(); frame.propertiesMs=state.propertiesMs;
                    result.propertyPoll.add(frame.propertiesMs); ++result.availableCheckedFrames;
                    available=state.available();
                    const double observedMs=elapsed(started);
                    if (afterAccepted) {
                        ++result.acceptedAvailabilityChecks;
                        if (available) {
                            if (result.firstAvailableMs<0) result.firstAvailableMs=observedMs;
                            if (unavailableSince>=0) {
                                result.longestUnavailableMs=std::max(result.longestUnavailableMs,observedMs-unavailableSince);
                                unavailableSince=-1;
                            }
                        } else {
                            ++result.unavailableObservations;
                            if (unavailableSince<0) unavailableSince=observedMs;
                        }
                    }
                }
                result.render.add(frame.renderMs); result.graphics.add(frame.graphicsMs); result.lock.add(frame.lockMs);
                samples << name << ',' << i << ',' << frame.updateMs << ',' << frame.renderMs << ',' << frame.graphicsMs << ',' << frame.lockMs << ',' << frame.propertiesMs
                    << ',' << accepted << ',' << signals.propertyRefreshes.load()-refreshesBefore << ',' << recovery << ',' << available << ',' << afterAccepted << '\n';
                if (Clock::now()>due+std::chrono::milliseconds(17)) due=Clock::now();
            }
            result.seconds=std::chrono::duration<double>(Clock::now()-started).count();
            if (unavailableSince>=0) result.longestUnavailableMs=std::max(result.longestUnavailableMs,result.seconds*1000-unavailableSince);
            result.acceptedUpdates=signals.acceptedUpdates.load()-acceptedBefore;
            result.propertyRefreshes=signals.propertyRefreshes.load()-refreshesBefore;
            result.after=snapshot();
            phases.push_back(result); samples.flush(); save();
            std::cout << name << ": " << result.json() << '\n';
        };
        warm(30);
        phase("steady_1pass",frames);
        phase("identical_updates",frames,[](int) { return true; });
        for (const char* key : {"mix","midtone","chroma","chroma_protection"}) {
            restore();
            phase(std::string("outlet_gradient_")+key,frames,[&](int i) {
                const double amount=(i%61)/60.0;
                p_obs_data_set_double(settings,key,std::string(key)=="mix" ? .2+.8*amount : std::string(key)=="chroma_protection" ? amount : .5+amount);
                return true;
            });
        }
        // First additions of NR feature handles must be measured before any
        // three-pass warm-up; later pass-count switches are explicitly warm.
        restore();
        for (int passes : {2,3}) {
            phase(std::string("cold_switch_passes_")+std::to_string(passes),30,[&](int i) {
                if (i) return false;
                p_obs_data_set_int(settings,"passes",passes); return true;
            },true);
        }
        restore(3); phase("steady_3passes",frames);
        for (const char* key : {"p1_tone","p2_tone","p3_tone"}) {
            restore(3); phase(std::string("nr_gradient_")+key,frames,[&](int i) { p_obs_data_set_double(settings,key,.5+(i%61)/60.0); return true; });
        }
        restore(); p_obs_data_set_bool(settings,"scaling",true); p_obs_data_set_int(settings,"resolution_control",1);
        std::vector<int> percentages;
        for (int percent=50;percent<100;percent+=percentStep) percentages.push_back(percent);
        percentages.push_back(100);
        for (int percent=100-percentStep;percent>50;percent-=percentStep) percentages.push_back(percent);
        percentages.push_back(50);
        phase("resolution_sweep_50_100_50",static_cast<int>(percentages.size()),[&](int i) { p_obs_data_set_int(settings,"percent",percentages[i]); return true; });
        phase("resolution_settle_50",60,{},true); warm();
        const auto settled=snapshot();
        const auto expectedInference=std::to_string(width/2)+"×"+std::to_string(height/2);
        require(settled.detail.find(expectedInference)!=std::string::npos,"Final manual scale did not become active after dragging");
        restore();
        for (const auto& option : {std::pair{"passes",2},std::pair{"passes",3},std::pair{"passes",1},
                                  std::pair{"model_preset",0},std::pair{"model_preset",2},std::pair{"model_preset",1},
                                  std::pair{"flow",1},std::pair{"flow",2},std::pair{"flow",0}}) {
            phase(std::string(std::string(option.first)=="passes" ? "warm_switch_" : "switch_")+option.first+'_'+std::to_string(option.second),30,[&](int i) {
                if (i) return false;
                p_obs_data_set_int(settings,option.first,option.second); return true;
            },true);
        }
        // Property creation/application is isolated from rendering/source updates.
        restore();
        Distribution getProperties,applyProperties;
        std::ofstream propertyReport(output/"properties.json");
        propertyReport << "{\"scope\":\"libobs property creation/application and callback costs; true modified results request full Qt form reconstruction in OBS\",\"controls\":[";
        bool firstProperty=true;
        for (const char* key : {"mix","midtone","p1_tone","passes","edit_pass","model_preset","percent","flow","af","ui_page","view","debug"}) {
            Distribution modified; int refreshRequests=0;
            const auto started=Clock::now(); auto* props=p_obs_source_properties(filter); getProperties.add(elapsed(started));
            auto* ui=p_obs_data_create_from_json(defaultsJson.c_str()); require(ui!=nullptr,"Clone property settings");
            p_obs_data_set_int(ui,"ui_page",4); p_obs_data_set_int(ui,"passes",3); p_obs_data_set_int(ui,"debug",1);
            const std::string original=p_obs_data_get_json(ui);
            for (int i=0;i<30;++i) {
                auto begin=Clock::now(); p_obs_properties_apply_settings(props,ui); applyProperties.add(elapsed(begin));
                auto* property=p_obs_properties_get(props,key); require(property!=nullptr,std::string("Missing property: ")+key);
                begin=Clock::now(); refreshRequests+=p_obs_property_modified(property,ui); modified.add(elapsed(begin));
            }
            require(std::string(p_obs_data_get_json(ui))==original,"Property callbacks rewrote settings");
            p_obs_data_release(ui); p_obs_properties_destroy(props);
            if (!firstProperty) propertyReport << ',';
            firstProperty=false;
            propertyReport << "{\"key\":" << quoted(key) << ",\"modifiedCallback\":" << modified.json()
                << ",\"fullFormRefreshRequests\":" << refreshRequests << ",\"settingsPreserved\":true}";
        }
        propertyReport << "],\"getProperties\":" << getProperties.json() << ",\"applySettings\":" << applyProperties.json() << "}\n";
        propertyReport.close(); save();
        p_signal_handler_disconnect(sourceSignals,"update",HostSignals::accepted,&signals);
        p_signal_handler_disconnect(sourceSignals,"update_properties",HostSignals::properties,&signals);
        p_obs_source_filter_remove(source,filter); p_obs_source_release(filter); p_obs_data_release(settings); p_obs_data_release(defaults);
        p_obs_source_dec_showing(source); p_obs_source_release(source);
        p_obs_enter_graphics(); p_gs_texrender_destroy(capture); p_obs_leave_graphics(); p_obs_shutdown();
        RemoveDllDirectory(cookie);
        std::cout << "Performance probe complete: " << output.string() << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
