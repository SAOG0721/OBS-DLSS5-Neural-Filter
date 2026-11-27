// SPDX-License-Identifier: GPL-3.0-only
#include "GpuBackend.h"
#include "NgxSession.h"
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace neural;
using namespace DirectX::PackedVector;
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
void hr(HRESULT value,const char* message) { require(SUCCEEDED(value),message); }
std::uint64_t timeNow() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
struct Fixture {
    Extent extent{512,288};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> input,output,staging;
    std::vector<unsigned short> pixels;
    float center=0;
    explicit Fixture() : pixels(extent.width*extent.height*4) {
        hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create performance contract device");
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=extent.width; desc.Height=extent.height;
        desc.MipLevels=desc.ArraySize=1; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        hr(device->CreateTexture2D(&desc,nullptr,&input),"Create performance input");
        hr(device->CreateTexture2D(&desc,nullptr,&output),"Create performance output");
        desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(device->CreateTexture2D(&desc,nullptr,&staging),"Create performance readback");
        upload(.1f);
    }
    void upload(float red) {
        for (unsigned y=0;y<extent.height;++y) for (unsigned x=0;x<extent.width;++x) {
            const auto i=(static_cast<size_t>(y)*extent.width+x)*4;
            pixels[i]=XMConvertFloatToHalf(red+(x/16%2)*.005f);
            pixels[i+1]=XMConvertFloatToHalf(.15f+(y/16%2)*.005f);
            pixels[i+2]=XMConvertFloatToHalf(.2f); pixels[i+3]=XMConvertFloatToHalf(.5f);
        }
        context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),extent.width*8,0); context->Flush();
    }
    float sample() {
        context->CopyResource(staging.Get(),output.Get()); context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{}; HRESULT result;
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        do { result=context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if (result==DXGI_ERROR_WAS_STILL_DRAWING) SwitchToThread();
        } while (result==DXGI_ERROR_WAS_STILL_DRAWING && Clock::now()<deadline);
        hr(result,"Read performance output");
        const auto* pixel=static_cast<const unsigned short*>(mapped.pData);
        const float red=XMConvertHalfToFloat(pixel[0]);
        const auto* middle=reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(mapped.pData)+
            (extent.height/2)*mapped.RowPitch)+(extent.width/2)*4;
        center=XMConvertHalfToFloat(middle[0]);
        require(pixel[3]==pixels[3],"Lightweight composition changed alpha");
        context->Unmap(staging.Get(),0); return red;
    }
};
// A different thread owns the SDK gate; a render-thread recursive try_lock
// would not be a valid contention test for std::timed_mutex.
struct HoldNgxGate {
    std::mutex mutex;
    std::condition_variable wake;
    bool locked=false,release=false;
    std::thread thread;
    explicit HoldNgxGate(const std::shared_ptr<NgxSession>& session) {
        thread=std::thread([this,session] {
            std::unique_lock gate(session->execution);
            std::unique_lock lock(mutex); locked=true; wake.notify_one();
            wake.wait(lock,[&] { return release; });
        });
        std::unique_lock lock(mutex); wake.wait(lock,[&] { return locked; });
    }
    ~HoldNgxGate() {
        { std::scoped_lock lock(mutex); release=true; }
        wake.notify_one(); thread.join();
    }
};
void nativePreparation(Fixture& fixture,const std::string& runtime) {
    ComPtr<IDXGIDevice> dxgi; hr(fixture.device.As(&dxgi),"Read native preparation adapter");
    ComPtr<IDXGIAdapter> adapter; hr(dxgi->GetAdapter(&adapter),"Get native preparation adapter");
    ComPtr<IDXGIAdapter1> adapter1; hr(adapter.As(&adapter1),"Query native preparation adapter");
    auto session=NgxSession::acquire(adapter1.Get(),runtime);
    auto backend=std::make_unique<GpuBackend>(BackendTestMode::Neural,3,BackendScheduling::CompletedFrames);
    Settings base; base.runtime=runtime;
    FrameKey key{1,1,1,fixture.extent};
    auto waitFor=[&](const Settings& settings,int percent,bool alternating) {
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        const auto requiredCompleted=backend->diagnostics().completedFrames+2;
        while (Clock::now()<deadline) {
            const bool visible=backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false);
            const auto diagnostic=backend->diagnostics();
            require(diagnostic.error.empty(),diagnostic.report());
            if (visible && diagnostic.outputAvailable && diagnostic.currentPercent==percent &&
                diagnostic.processingMode==(alternating ? 1 : 0) && diagnostic.completedFrames>=requiredCompleted && diagnostic.outputAgeMs<100) {
                fixture.sample(); return;
            }
            ++key.cycle; Sleep(3);
        }
        throw std::runtime_error("Native prepared pipeline did not become active: "+backend->diagnostics().report());
    };
    waitFor(base,100,false);
    // Complete this fixed key before the gate test; only outlet changes should
    // be needed while the SDK itself is unavailable to the render thread.
    fixture.sample(); Sleep(20);
    backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),base,key,timeNow(),false);
    fixture.sample(); Sleep(20);
    Settings target=base;
    {
        HoldNgxGate hold(session);
        const auto before=backend->diagnostics();
        for (int i=0;i<10;++i) {
            base.midtone=.4f+i*.1f;
            require(backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),base,key,timeNow(),false),
                "Pure outlet composition waited for the NGX gate");
            fixture.sample(); Sleep(3);
        }
        const auto after=backend->diagnostics();
        require(after.completedFrames>before.completedFrames && after.evaluations==before.evaluations,
            "Native outlet composition used the NGX gate or repeated NR: "+after.report());
        for (int percent : {95,80,65}) {
            target=base; target.scaling=true; target.percent=percent; target.modelPreset=percent==80 ? 2 : 0;
            target.flow=percent==80 ? 1 : 0; target.passes=percent==80 ? 3 : 2; target.processingMode=percent==80 ? 1 : 0;
            ++key.cycle; const auto start=Clock::now();
            require(backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),target,key,timeNow(),false),
                "Pending resources lost the previous completed output");
            require(Clock::now()-start<std::chrono::milliseconds(80),"Render waited for background preparation");
            const auto diagnostic=backend->diagnostics();
            require(diagnostic.currentPercent==100 && diagnostic.processingMode==0 && diagnostic.error.empty(),
                "Unprepared structural parameters leaked into the active pipeline: "+diagnostic.report());
        }
        ++key.cycle;
        require(backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),base,key,timeNow(),false),
            "Cancelling preparation lost the healthy pipeline");
        target=base; target.scaling=true; target.percent=50; target.modelPreset=2;
        target.flow=2; target.nvQuality=2; target.passes=2; target.processingMode=1;
        ++key.cycle; const auto start=Clock::now();
        require(backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),target,key,timeNow(),false),
            "Latest preparation request lost the healthy pipeline");
        require(Clock::now()-start<std::chrono::milliseconds(80),"Latest resource request joined the preparation worker");
    }
    waitFor(target,50,true);
    require(backend->diagnostics().inference==inferenceExtent(fixture.extent,target) &&
        backend->diagnostics().flow.starts_with("NVOF"),"Latest model/extent/flow/mode request was not adopted");
    const auto retryStart=Clock::now(); backend->retry();
    require(Clock::now()-retryStart<std::chrono::milliseconds(80),"Retry destroyed resources on the render thread");
    waitFor(target,50,true);
    {
        HoldNgxGate hold(session);
        Settings pending=target; pending.percent=75; pending.modelPreset=0;
        ++key.cycle;
        backend->process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),pending,key,timeNow(),false);
        const auto start=Clock::now(); backend.reset();
        require(Clock::now()-start<std::chrono::milliseconds(80),"Deleting a preparing filter joined/released NGX on render");
    }
    const auto cleanupDeadline=Clock::now()+std::chrono::seconds(10);
    while (session.use_count()>1 && Clock::now()<cleanupDeadline) Sleep(5);
    require(session.use_count()==1,"Cancelled preparation retained its NGX resources after retirement");
    std::cout << "Native completed-frame preparation, contended NGX gate, latest/cancelled requests, retry, deferred deletion and alpha passed\n";
}
int main(int argc,char** argv) {
    try {
        Fixture fixture;
        if (argc>2 && std::string(argv[1])=="--prepare-native") { nativePreparation(fixture,argv[2]); return 0; }
        for (int method : {0,1,2}) for (int interval : {2,3,4}) {
            GpuBackend backend(BackendTestMode::ControlledDelta,3000);
            Settings settings; settings.processingMode=1; settings.updateInterval=interval;
            settings.residualTransfer=method ? 1 : 0; settings.flow=method; settings.af=method ? 2 : 0;
            FrameKey key{1,1,1,fixture.extent}; std::uint64_t now=1000000000;
            float previous=0;
            for (int frame=0;frame<12;++frame) {
                fixture.upload(.1f+frame*.001f);
                const auto before=backend.diagnostics();
                require(backend.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,now,frame==0),backend.diagnostics().report());
                float value=fixture.sample();
                const auto after=backend.diagnostics();
                require(after.evaluations==static_cast<unsigned>(frame/interval+1),"Full NR cadence differs from independent OBS cycles: "+after.report());
                if (method==0 && frame%interval!=0) require(value>previous+.0005f,"Skipped NR held an old whole frame instead of current base");
                const auto commits=after.temporalCommits;
                for (int view=0;view<2;++view) {
                    require(backend.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,now,false),backend.diagnostics().report());
                    require(fixture.sample()==value,"Two same-cycle views chose different lightweight output");
                    require(backend.diagnostics().evaluations==after.evaluations && backend.diagnostics().temporalCommits==commits,
                        "Same-cycle preview advanced inference or temporal history");
                }
                if (frame%interval!=0) {
                    const auto beforeOutlet=value;
                    settings.midtone=.7f+.05f*(frame%interval); settings.chromaProtection=.4f;
                    require(backend.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,now,false),backend.diagnostics().report());
                    value=fixture.sample();
                    require(backend.diagnostics().evaluations==after.evaluations,"Outlet adjustment repeated NR");
                    require(value>.1f+frame*.001f+.001f,"Same-key outlet recomposition cleared the retained enhancement; frame="+
                        std::to_string(frame)+"; firstBefore="+std::to_string(beforeOutlet)+"; firstAfter="+std::to_string(value)+
                        "; centerAfter="+std::to_string(fixture.center)+"; "+backend.diagnostics().report());
                }
                previous=value; ++key.cycle; now+=16666667;
            }
            require(backend.diagnostics().lightweightComposites>=12,"Intermediate inputs did not get lightweight compositions");
            std::cout << "interval=" << interval << "; flow=" << method << "; full=" << backend.diagnostics().fullUpdates
                      << "; light=" << backend.diagnostics().lightweightComposites << " passed\n";
        }
        // A target can change pixels within the full-update cycle, including
        // alongside an outlet edit. Its new epoch must use a freshly captured
        // inference proxy rather than relabeling the old pair.
        for (bool editOutlet : {false,true}) {
            GpuBackend backend(BackendTestMode::ControlledDelta,3000);
            Settings sameSettings; sameSettings.processingMode=1; sameSettings.updateInterval=4;
            FrameKey sameKey{1,1,1,fixture.extent}; fixture.upload(.1f);
            require(backend.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),sameSettings,sameKey,1000000000,true),backend.diagnostics().report());
            fixture.sample(); const auto evaluations=backend.diagnostics().evaluations;
            fixture.upload(.2f); if (editOutlet) sameSettings.midtone=.7f;
            for (int poll=0;poll<3;++poll) {
                require(backend.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),sameSettings,sameKey,1000000000,false),backend.diagnostics().report());
                fixture.sample();
            }
            const auto replacement=fixture.sample();
            require(backend.diagnostics().evaluations==evaluations+1,"Same-cycle pixel replacement did not force exactly one fresh NR update");
            GpuBackend fresh(BackendTestMode::ControlledDelta,3000);
            require(fresh.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),sameSettings,sameKey,1000000000,true),fresh.diagnostics().report());
            const auto expected=fixture.sample();
            require(std::abs(replacement-expected)<.0006f,"Same-cycle replacement used an old proxy/reference under a new epoch");
        }
        // Private inference is deliberately held on a queue fence. The parallel
        // light queue must keep changing the current base on the OBS context.
        GpuBackend realtime(BackendTestMode::ControlledDelta,3,BackendScheduling::CompletedFrames);
        Settings settings; settings.processingMode=1; settings.updateInterval=2;
        FrameKey key{1,1,1,fixture.extent}; bool visible=false;
        for (int frame=0;frame<8;++frame) {
            fixture.upload(.1f+frame*.001f);
            realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),frame==0);
            Sleep(5); visible=realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false) || visible;
            ++key.cycle;
        }
        require(visible,"Parallel light queue did not initialize"); fixture.sample();
        // Submit/retire the current key before varying controls. Each following
        // call changes Settings while reusing the exact same input/key.
        realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false);
        Sleep(20);
        realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false);
        fixture.sample(); Sleep(20);
        const auto beforeDrag=realtime.diagnostics();
        const float beforeDragPixel=fixture.sample(); bool dragChangedOutput=false;
        float lastDragPixel=beforeDragPixel;
        for (int step=0;step<12;++step) {
            settings.midtone=(step&1) ? 1.8f : .2f;
            require(realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false),
                "Continuous outlet update lost the completed light output");
            const float dragPixel=fixture.sample();
            lastDragPixel=dragPixel;
            dragChangedOutput=dragChangedOutput || std::abs(dragPixel-beforeDragPixel)>.0005f;
            Sleep(2);
        }
        const auto afterDrag=realtime.diagnostics();
        require(afterDrag.inputComparisons>beforeDrag.inputComparisons,"Light outlet drag bypassed input pixel verification");
        require(afterDrag.lightweightComposites>beforeDrag.lightweightComposites && dragChangedOutput,
            "Light outlet updates discarded pixel validation and starved composition; beforeLight="+
            std::to_string(beforeDrag.lightweightComposites)+"; afterLight="+std::to_string(afterDrag.lightweightComposites)+
            "; changed="+std::to_string(dragChangedOutput)+"; beforePixel="+std::to_string(beforeDragPixel)+
            "; lastPixel="+std::to_string(lastDragPixel)+"; beforeComparisons="+std::to_string(beforeDrag.inputComparisons)+
            "; beforeEvaluations="+std::to_string(beforeDrag.evaluations)+"; beforeTemporal="+
            std::to_string(beforeDrag.temporalCommits)+"; "+afterDrag.report());
        require(afterDrag.evaluations==beforeDrag.evaluations && afterDrag.temporalCommits==beforeDrag.temporalCommits,
            "Same-key light outlet drag advanced NR or temporal observations");
        settings.midtone=1; ++key.cycle;
        realtime.setTestDelay(100);
        const auto baseUpdates=realtime.diagnostics().fullUpdates;
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        while (realtime.diagnostics().fullUpdates==baseUpdates && Clock::now()<deadline) {
            realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false); ++key.cycle; Sleep(2);
        }
        require(realtime.diagnostics().fullUpdates>baseUpdates,"Delayed inference was not submitted");
        const auto start=Clock::now(); const auto first=fixture.sample(); float last=first;
        const auto firstReadbackMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        double maximumStepMs=0;
        const auto fullPending=realtime.diagnostics().fullUpdates;
        for (int frame=0;frame<5;++frame) {
            const auto frameStart=Clock::now();
            fixture.upload(.13f+frame*.002f);
            const bool ready=realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false);
            if (ready) {
                last=fixture.sample(); const auto commits=realtime.diagnostics().temporalCommits;
                require(realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false),"Extra preview lost completed light output");
                require(fixture.sample()==last,"Same-cycle output slot was overwritten");
                require(realtime.diagnostics().temporalCommits==commits,"Extra preview committed a new temporal observation");
            }
            ++key.cycle;
            maximumStepMs=std::max(maximumStepMs,std::chrono::duration<double,std::milli>(Clock::now()-frameStart).count());
        }
        require(Clock::now()-start<std::chrono::milliseconds(80),"OBS immediate context waited for pending full inference; totalMs="+
            std::to_string(std::chrono::duration<double,std::milli>(Clock::now()-start).count()));
        require(last>first+.01f,"Current base froze while full inference was pending");
        require(realtime.diagnostics().fullUpdates==fullPending,"Private inference delay did not cover the light-queue test");
        realtime.setTestDelay(0); Sleep(110);
        std::cout << "Parallel current-base updates, immutable same-cycle output, no inference wait and alpha passed; firstReadbackMs="
                  << firstReadbackMs << "; maximumStepMs=" << maximumStepMs << '\n';
        // Repeated producer/consumer completions exercise both immutable pair
        // slots while extra views verify that completed OBS slots stay frozen.
        for (int frame=0;frame<60;++frame) {
            fixture.upload(.1f+(frame%11)*.001f);
            const bool shown=realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false);
            if (shown) {
                const auto value=fixture.sample(); const auto commits=realtime.diagnostics().temporalCommits;
                for (int view=0;view<3;++view) {
                    realtime.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),settings,key,timeNow(),false);
                    require(fixture.sample()==value,"Rapid producer completion overwrote the same-cycle consumer slot");
                    require(realtime.diagnostics().temporalCommits==commits,"Rapid completion advanced history from another preview");
                }
            }
            ++key.cycle; Sleep(1);
        }
        GpuBackend adaptive(BackendTestMode::ControlledDelta,3,BackendScheduling::CompletedFrames);
        Settings autoSettings; autoSettings.scaling=true; autoSettings.resolutionControl=0; autoSettings.percent=63;
        FrameKey autoKey{1,1,1,fixture.extent};
        bool warm=false;
        for (int frame=0;frame<30;++frame) {
            warm=adaptive.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),autoSettings,autoKey,timeNow(),frame==0) || warm;
            ++autoKey.cycle; Sleep(3);
        }
        require(warm,"Adaptive backend did not initialize"); adaptive.setTestDelay(100);
        const auto adaptationDeadline=Clock::now()+std::chrono::seconds(4); bool lowered=false;
        while (Clock::now()<adaptationDeadline) {
            require(adaptive.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),autoSettings,autoKey,timeNow(),false),adaptive.diagnostics().report());
            ++autoKey.cycle; Sleep(3);
            if (adaptive.diagnostics().currentPercent<100) { lowered=true; break; }
        }
        require(lowered,"Overload did not warm and commit a lower safe adaptive tier: "+adaptive.diagnostics().report());
        require(adaptive.diagnostics().currentPercent%5==0 && safeExtent(adaptive.diagnostics().inference),"Adaptive switch selected an unsafe tier");
        require(autoSettings.percent==63,"Adaptive backend changed saved manual percentage");
        adaptive.setTestDelay(0); autoSettings.resolutionControl=1;
        const auto manualDeadline=Clock::now()+std::chrono::seconds(3);
        while (adaptive.diagnostics().currentPercent!=63 && Clock::now()<manualDeadline) {
            adaptive.process(fixture.device.Get(),fixture.input.Get(),fixture.output.Get(),autoSettings,autoKey,timeNow(),false);
            ++autoKey.cycle; Sleep(3);
        }
        require(adaptive.diagnostics().currentPercent==63 && adaptive.diagnostics().inference==inferenceExtent(fixture.extent,autoSettings),
            "Leaving adaptive resolution did not restore the saved manual scale");
        std::cout << "Source-epoch recapture, rapid pair publication, warmed adaptive switch and manual restoration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
