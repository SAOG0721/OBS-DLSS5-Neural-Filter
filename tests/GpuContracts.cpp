// SPDX-License-Identifier: GPL-3.0-only
#include "GpuBackend.h"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <timeapi.h>
#include <vector>
using namespace neural;
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void hr(HRESULT value,const char* message) { check(SUCCEEDED(value),message); }
std::vector<unsigned short> read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture) {
    auto desc=D3D11_TEXTURE2D_DESC{}; texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr(device->CreateTexture2D(&desc,nullptr,&staging),"Create test readback");
    context->CopyResource(staging.Get(),texture); context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    HRESULT result;
    do {
        result=context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if (result==DXGI_ERROR_WAS_STILL_DRAWING) Sleep(1);
    } while (result==DXGI_ERROR_WAS_STILL_DRAWING && std::chrono::steady_clock::now()<deadline);
    hr(result,"Bounded GPU test readback");
    std::vector<unsigned short> pixels(static_cast<size_t>(desc.Width)*desc.Height*4);
    for (unsigned y=0;y<desc.Height;++y)
        std::memcpy(pixels.data()+static_cast<size_t>(y)*desc.Width*4,
                    static_cast<unsigned char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,desc.Width*8);
    context->Unmap(staging.Get(),0); return pixels;
}
int main(int argc,char** argv) {
    try {
        const bool benchmark=argc>1 && std::string(argv[1])=="--benchmark";
        const bool combinations=argc>1 && std::string(argv[1])=="--combinations";
        const bool real=benchmark || combinations || (argc>1 && std::string(argv[1])=="--neural");
        Extent extent{1280,720};
        if (argc>3) extent={static_cast<unsigned>(std::stoul(argv[2])),static_cast<unsigned>(std::stoul(argv[3]))};
        ComPtr<IDXGIFactory1> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (unsigned i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId==0x10de) break;
            adapter.Reset();
        }
        check(static_cast<bool>(adapter),"No NVIDIA GPU for native tests");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature;
        hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,&feature,&context),"Create GPU test D3D11 device");
        std::vector<unsigned short> pixels(static_cast<size_t>(extent.width)*extent.height*4);
        for (unsigned y=0;y<extent.height;++y) for (unsigned x=0;x<extent.width;++x) {
            const float a=(x%4)*.25f;
            const size_t index=(static_cast<size_t>(y)*extent.width+x)*4;
            const float lum=.02f+static_cast<float>((x*17+y*13)%211)/300.f;
            pixels[index]=XMConvertFloatToHalf(lum*a); pixels[index+1]=XMConvertFloatToHalf((.15f+lum*.7f)*a);
            pixels[index+2]=XMConvertFloatToHalf((.1f+lum*.6f)*a); pixels[index+3]=XMConvertFloatToHalf(a);
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=extent.width; desc.Height=extent.height;
        desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA upload{}; upload.pSysMem=pixels.data(); upload.SysMemPitch=extent.width*8;
        ComPtr<ID3D11Texture2D> input,output;
        hr(device->CreateTexture2D(&desc,&upload,&input),"Create GPU test input");
        hr(device->CreateTexture2D(&desc,nullptr,&output),"Create GPU test output");
        Settings settings; if (real && argc>4) settings.runtime=argv[4];
        if (benchmark && argc>7) {
            settings.percent=std::stoi(argv[7]); settings.scaling=settings.percent!=100;
        }
        GpuBackend backend(real ? BackendTestMode::Neural : BackendTestMode::Identity,benchmark ? 3 : 3000);
        FrameKey key{1,0,1,extent}; std::uint64_t now=1000000000;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,true),backend.diagnostics().error.c_str());
        auto result=read(device.Get(),context.Get(),output.Get());
        if (combinations) {
            auto verify=[&](const Settings& candidate,bool newFrame=true) {
                if (newFrame) { ++key.cycle; now+=16666667; }
                check(backend.process(device.Get(),input.Get(),output.Get(),candidate,key,now,false),backend.diagnostics().report().c_str());
                const auto outputPixels=read(device.Get(),context.Get(),output.Get());
                for (size_t i=0;i<pixels.size();i+=4) {
                    check(outputPixels[i+3]==pixels[i+3],"Combination changed alpha");
                    for (int c=0;c<3;++c) check(std::isfinite(XMConvertHalfToFloat(outputPixels[i+c])),"Combination produced non-finite RGB");
                }
                check(backend.diagnostics().source==extent,"Combination changed source output size");
                return outputPixels;
            };
            for (int percent : {25,50,75,100}) {
                settings.scaling=true; settings.percent=percent;
                verify(settings);
                check(backend.diagnostics().inference==inferenceExtent(extent,settings),"Inference scale did not reach backend");
                std::cout << "scale=" << percent << "% passed\n";
            }
            settings.scaling=false;
            settings.advanced=true; settings.hue=.4f; settings.darkProtect=.3f;
            settings.brightProtect=.3f; settings.compression=.5f; settings.low=.6f; settings.high=1.2f;
            verify(settings);
            const auto beforeOutlet=backend.diagnostics().evaluations;
            settings.chroma=.4f; settings.lightness=.6f; settings.darken=.5f; settings.brighten=.5f;
            verify(settings,false);
            check(backend.diagnostics().evaluations==beforeOutlet,"Advanced outlet controls repeated NR");
            settings.low=0; settings.high=0;
            check(verify(settings,false)==pixels,"Zero low/high correction did not preserve original");
            settings.low=1; settings.high=1;
            for (int style=0;style<3;++style) {
                settings.pass[0]={style,.7f,1.25f,.8f,.5f,true,true};
                verify(settings); std::cout << "seven pass controls style=" << style << " passed\n";
            }
            for (int method : {0,1,2}) for (int af=0;af<5;++af) {
                settings.flow=method; settings.af=af;
                verify(settings); verify(settings); verify(settings);
                const auto before=backend.diagnostics(); verify(settings,false);
                const auto after=backend.diagnostics();
                check(after.evaluations==before.evaluations && after.temporalCommits==before.temporalCommits,"Combined flow/AF duplicate advanced history");
                if (method) check(after.flow.starts_with(method==1 ? "AMDOF" : "NVOF"),"Combined flow fell back unexpectedly");
                std::cout << "NR+flow=" << method << "+AF=" << af << " passed\n";
            }
            settings.passes=3; verify(settings);
            std::cout << backend.diagnostics().report() << "\nReal NR scales, pass controls, advanced zero correction and all flow/AF combinations passed.\n";
            return 0;
        }
        if (benchmark) {
            timeBeginPeriod(1);
            const int fps=argc>5 ? std::stoi(argv[5]) : 60;
            const int frames=argc>6 ? std::stoi(argv[6]) : 300;
            const int percent=argc>7 ? std::stoi(argv[7]) : 100;
            settings.scaling=percent!=100; settings.percent=percent;
            std::vector<double> gpu,cpu,wait;
            const auto start=std::chrono::steady_clock::now();
            for (int frame=0;frame<frames;++frame) {
                const auto due=start+std::chrono::nanoseconds(static_cast<long long>(frame)*1000000000ll/fps);
                std::this_thread::sleep_until(due);
                key.cycle++;
                now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (!backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false))
                    throw std::runtime_error(backend.diagnostics().report());
                const auto diagnostics=backend.diagnostics();
                if (frame>10) { gpu.push_back(diagnostics.gpuTotalMs); cpu.push_back(diagnostics.cpuSubmitMs); wait.push_back(diagnostics.waitMs); }
            }
            read(device.Get(),context.Get(),output.Get());
            auto percentile=[](std::vector<double> values,double p) {
                std::sort(values.begin(),values.end());
                return values.at(static_cast<size_t>(p*(values.size()-1)));
            };
            std::cout << backend.diagnostics().report() << "\nbackend-only steady probe: fps=" << fps
                      << "; frames=" << frames << "; percent=" << percent
                      << "; gpuP95=" << percentile(gpu,.95) << "; gpuP99=" << percentile(gpu,.99)
                      << "; cpuP95=" << percentile(cpu,.95) << "; allocatorWaitP95=" << percentile(wait,.95)
                      << "\nOBS composition, game and NVENC are not included.\n";
            timeEndPeriod(1);
            return 0;
        }
        if (!real) check(result==pixels,"Identity backend altered original RGB or alpha");
        for (size_t i=0;i<pixels.size();i+=4) {
            check(result[i+3]==pixels[i+3],"Alpha changed");
            if (!pixels[i+3]) check(result[i]==0 && result[i+1]==0 && result[i+2]==0,"Transparent RGB leaked");
        }
        auto initial=backend.diagnostics().evaluations;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Cached render failed");
        check(backend.diagnostics().evaluations==initial,"Duplicate render repeated Evaluate");
        read(device.Get(),context.Get(),output.Get());
        // Same OBS cycle, real input change without an update signal must not
        // return the stale cached output.
        pixels[4]=XMConvertFloatToHalf(.1f);
        context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),extent.width*8,0);
        const auto beforeChange=backend.diagnostics().evaluations;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Same-cycle changed input failed");
        check(backend.diagnostics().evaluations==beforeChange+1,"Same-cycle input change returned stale cache");
        read(device.Get(),context.Get(),output.Get());
        initial=beforeChange+1;
        settings.chroma=.5f;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Outlet update failed");
        check(backend.diagnostics().evaluations==initial,"Outlet update repeated Evaluate");
        read(device.Get(),context.Get(),output.Get());
        settings.passes=2;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Two-pass suffix failed");
        check(backend.diagnostics().evaluations==initial+1,"Second pass recomputed prefix");
        read(device.Get(),context.Get(),output.Get());
        settings.passes=3;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Three-pass suffix failed");
        check(backend.diagnostics().evaluations==initial+2,"Suffix cache recomputed prefix");
        read(device.Get(),context.Get(),output.Get());
        settings.passes=1;
        check(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Reducing pass count failed");
        check(backend.diagnostics().evaluations==initial+2,"Reducing pass count repeated Evaluate");
        read(device.Get(),context.Get(),output.Get());
        std::cout << backend.diagnostics().report() << "\nGpuContracts: identity/alpha/cache/outlet/suffix checks passed\n";
        if (real) {
            bool changed=false;
            for (size_t i=0;i<pixels.size();i+=4) if (pixels[i+3] && (result[i]!=pixels[i] || result[i+1]!=pixels[i+1] || result[i+2]!=pixels[i+2])) changed=true;
            check(changed,"Neural backend produced no normal-view net correction");
            std::cout << "Real NR normal-view net correction confirmed on synthetic input; real-content acceptance is separate.\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
