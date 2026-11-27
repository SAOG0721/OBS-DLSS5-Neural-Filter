// SPDX-License-Identifier: GPL-3.0-only
#include "GpuBackend.h"
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace neural;
using namespace DirectX::PackedVector;
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
void require(bool okay,const std::string& message) { if (!okay) throw std::runtime_error(message); }
void hr(HRESULT result,const char* message) { require(SUCCEEDED(result),message); }
std::uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
int main() {
    try {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create realtime test device");
        Extent extent{256,144};
        std::vector<unsigned short> pixels(extent.width*extent.height*4);
        for (size_t i=0;i<pixels.size();i+=4) {
            pixels[i]=XMConvertFloatToHalf(.2f); pixels[i+1]=XMConvertFloatToHalf(.15f);
            pixels[i+2]=XMConvertFloatToHalf(.1f); pixels[i+3]=XMConvertFloatToHalf(.5f);
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=extent.width; desc.Height=extent.height;
        desc.MipLevels=desc.ArraySize=1; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA upload{pixels.data(),extent.width*8,0};
        ComPtr<ID3D11Texture2D> input,output,staging;
        hr(device->CreateTexture2D(&desc,&upload,&input),"Create realtime input");
        hr(device->CreateTexture2D(&desc,nullptr,&output),"Create realtime output");
        desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(device->CreateTexture2D(&desc,nullptr,&staging),"Create realtime readback");
        auto sample=[&] {
            const auto start=Clock::now(); context->CopyResource(staging.Get(),output.Get()); context->Flush();
            D3D11_MAPPED_SUBRESOURCE mapped{}; HRESULT result;
            do { result=context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                if(result==DXGI_ERROR_WAS_STILL_DRAWING) Sleep(1);
            } while(result==DXGI_ERROR_WAS_STILL_DRAWING && Clock::now()-start<std::chrono::seconds(2));
            hr(result,"Read realtime completed output");
            auto* value=static_cast<unsigned short*>(mapped.pData);
            const float red=XMConvertHalfToFloat(value[0]);
            require(value[3]==pixels[3],"Completed output changed alpha"); context->Unmap(staging.Get(),0);
            return red;
        };
        GpuBackend backend(BackendTestMode::ControlledDelta,3,BackendScheduling::CompletedFrames);
        Settings settings; settings.af=1; FrameKey key{1,1,1,extent};
        auto untilReady=[&] {
            const auto deadline=Clock::now()+std::chrono::seconds(3);
            while (Clock::now()<deadline) {
                const bool displayed=backend.process(device.Get(),input.Get(),output.Get(),settings,key,now(),false);
                const auto diagnostic=backend.diagnostics();
                // The first capture timestamp predates cold device/shader
                // initialization. Establish a recent completed observation
                // before testing a separate 100 ms private-queue overload.
                if (displayed && diagnostic.outputAgeMs<100) return;
                require(diagnostic.error.empty(),diagnostic.report()); Sleep(2); ++key.cycle;
            }
            throw std::runtime_error("Completed output did not become available: "+backend.diagnostics().report());
        };
        backend.process(device.Get(),input.Get(),output.Get(),settings,key,now(),true); untilReady(); sample();
        // A delayed private queue must not block reading the completed output
        // on the OBS-style D3D11 context, or alternate between it and raw input.
        backend.setTestDelay(100); ++key.cycle;
        require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now(),false),"Warm pipeline did not retain a completed result");
        const auto before=backend.diagnostics(); const auto start=Clock::now();
        const auto retained=sample();
        require(Clock::now()-start<std::chrono::milliseconds(80),"OBS context waited for current private inference");
        for(int repeat=0;repeat<8;++repeat) {
            const bool displayed=backend.process(device.Get(),input.Get(),output.Get(),settings,key,now(),false);
            require(displayed,"Busy pipeline returned original instead of ready output; cycle="+std::to_string(key.cycle)+
                "; repeat="+std::to_string(repeat)+"; elapsedMs="+
                std::to_string(std::chrono::duration<double,std::milli>(Clock::now()-start).count())+"; "+backend.diagnostics().report());
            require(backend.diagnostics().displayedFrame==before.displayedFrame,"Two views chose different results in one cycle");
            require(backend.diagnostics().temporalCommits==before.temporalCommits,"Busy draw advanced temporal history");
            require(backend.diagnostics().resets==before.resets,"Busy draw reset temporal history");
            require(sample()==retained,"Busy output flickered"); Sleep(2);
        }
        Sleep(110); backend.setTestDelay(0); ++key.cycle; untilReady(); sample();
        require(backend.diagnostics().completedFrames>=2,"Overload did not recover");
        // Keep the same cycle while replacing pixels with no source signal.
        // Asynchronous exact-input comparison must eventually invalidate it.
        for(size_t i=0;i<pixels.size();i+=4) pixels[i]=XMConvertFloatToHalf(.35f);
        context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),extent.width*8,0); context->Flush();
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        bool changed=false;
        while(Clock::now()<deadline) {
            backend.process(device.Get(),input.Get(),output.Get(),settings,key,now(),false);
            if(sample()>.3f) {changed=true;break;} Sleep(2);
        }
        require(changed,"Same-cycle pixel change reused stale output indefinitely");
        require(backend.diagnostics().inputComparisons>0,"Exact input verification was not performed");
        // Keep changing an outlet while a same-key pixel comparison is pending.
        // The comparison belongs to the captured input, not the slider value
        // that happened to be selected when it was submitted.
        const auto dragStart=Clock::now();
        const auto beforeDrag=backend.diagnostics();
        const float beforeDragPixel=sample(); bool dragChangedOutput=false;
        for (int step=0;step<12;++step) {
            // Asynchronous verification may skip intermediate mouse values.
            // Keep consumed values distinct instead of locking a two-value
            // toggle to the verification/submission cadence and aging a cache hit.
            settings.midtone=.2f+step*.14f;
            const bool displayed=backend.process(device.Get(),input.Get(),output.Get(),settings,key,now(),false);
            require(displayed,"Continuous same-key outlet update lost the completed output; step="+std::to_string(step)+
                "; elapsedMs="+std::to_string(std::chrono::duration<double,std::milli>(Clock::now()-dragStart).count())+
                "; beforeDragPixel="+std::to_string(beforeDragPixel)+"; before drag: "+beforeDrag.report()+
                "; after drag: "+backend.diagnostics().report());
            const float dragPixel=sample();
            dragChangedOutput=dragChangedOutput || std::abs(dragPixel-beforeDragPixel)>.0005f;
            Sleep(2);
        }
        const auto afterDrag=backend.diagnostics();
        require(afterDrag.inputComparisons>beforeDrag.inputComparisons,"Outlet drag bypassed input pixel verification");
        require(afterDrag.completedFrames>beforeDrag.completedFrames && dragChangedOutput,
            "Changing outlet values discarded pixel validation and starved composition: "+afterDrag.report());
        require(afterDrag.evaluations==beforeDrag.evaluations && afterDrag.temporalCommits==beforeDrag.temporalCommits,
            "Same-key outlet drag advanced NR or temporal observations");
        std::cout << "Realtime completed-result display, two-view consistency, bounded busy handling, AF retention, recovery, alpha and same-cycle pixel invalidation passed\n"
                  << backend.diagnostics().report() << '\n';
        return 0;
    } catch(const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
}
