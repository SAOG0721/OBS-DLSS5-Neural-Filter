// SPDX-License-Identifier: GPL-3.0-only
#include "GpuBackend.h"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace neural;
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
void require(bool value,const std::string& message) {
    if (!value) { std::cerr << "First contract failure: " << message << std::endl; throw std::runtime_error(message); }
}
int main(int argc,char** argv) {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception)->LONG {
        HMODULE module=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(exception->ExceptionRecord->ExceptionAddress),&module);
        wchar_t path[32768]{}; GetModuleFileNameW(module,path,32768);
        std::wcerr << L"Native fault module=" << path << L"; code=" << std::hex <<
            exception->ExceptionRecord->ExceptionCode << L"; address=" <<
            exception->ExceptionRecord->ExceptionAddress << std::endl;
        return EXCEPTION_EXECUTE_HANDLER;
    });
    try {
        const int method=argc>1 ? std::stoi(argv[1]) : 1;
        std::cout.setf(std::ios::unitbuf);
        const int quality=argc>2 ? std::stoi(argv[2]) : (method==1 ? 1 : 2);
        const Extent extent{512,288};
        ComPtr<IDXGIFactory1> factory; require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"Create DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (unsigned i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (d.VendorId==0x10de && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
        require(adapter!=nullptr,"NVIDIA native flow test GPU unavailable");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Create flow-test D3D11 device");
        D3D11_TEXTURE2D_DESC d{}; d.Width=extent.width; d.Height=extent.height; d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.ArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> input,output;
        require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&input)) && SUCCEEDED(device->CreateTexture2D(&d,nullptr,&output)),"Create flow-test textures");
        auto upload=[&](int shiftX,int shiftY) {
            std::vector<unsigned short> pixels(static_cast<size_t>(extent.width)*extent.height*4);
            for (unsigned y=0;y<extent.height;++y) for (unsigned x=0;x<extent.width;++x) {
                const int px=static_cast<int>(x)-shiftX,py=static_cast<int>(y)-shiftY;
                const float v=.4f+.15f*std::sin(px*.097f)+.13f*std::cos(py*.11f)+.12f*std::sin((px+py)*.065f);
                const size_t index=(static_cast<size_t>(y)*extent.width+x)*4;
                pixels[index]=XMConvertFloatToHalf(v); pixels[index+1]=XMConvertFloatToHalf(v*.85f);
                pixels[index+2]=XMConvertFloatToHalf(v*.65f); pixels[index+3]=XMConvertFloatToHalf(1);
            }
            context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),extent.width*8,0);
        };
        Settings settings; settings.flow=method; settings.amdQuality=quality; settings.nvQuality=quality; settings.af=2;
        GpuBackend backend(BackendTestMode::ControlledDelta);
        upload(0,0);
        std::uint64_t now=1000000000; FrameKey key{1,0,1,extent};
        require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,true),backend.diagnostics().error);
        backend.inspectMotion();
        auto initial=backend.diagnostics();
        require(initial.flow.starts_with(method==1 ? "AMDOF" : "NVOF"),"Selected flow fell back: "+initial.flow);
        upload(8,0); now+=16666667; key.cycle++;
        require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),backend.diagnostics().error);
        const auto horizontal=backend.inspectMotion();
        auto average=[&](const auto& vectors) {
            std::array<double,2> mean{}; unsigned count=0;
            for (unsigned y=64;y<extent.height-64;++y) for (unsigned x=64;x<extent.width-64;++x) {
                auto v=vectors[static_cast<size_t>(y)*extent.width+x];
                if (!std::isfinite(v[0]) || !std::isfinite(v[1])) throw std::runtime_error("Non-finite motion");
                mean[0]+=v[0]; mean[1]+=v[1]; ++count;
            }
            mean[0]/=count; mean[1]/=count; return mean;
        };
        const auto x=average(horizontal);
        std::cout << backend.diagnostics().flow << "\nrightward8px meanMotion=" << x[0] << ',' << x[1] << '\n';
        require(std::abs(x[0]+8)<2 && std::abs(x[1])<2,"Horizontal direction or pixel-unit contract failed");
        upload(8,8); now+=16666667; key.cycle++;
        require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),backend.diagnostics().error);
        const auto y=average(backend.inspectMotion());
        std::cout << "downward8px meanMotion=" << y[0] << ',' << y[1] << '\n';
        require(std::abs(y[1]+8)<2 && std::abs(y[0])<2,"Vertical direction or pixel-unit contract failed");
        const auto evaluations=backend.diagnostics().evaluations;
        require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Repeat flow input failed");
        require(backend.diagnostics().evaluations==evaluations,"Repeat flow input advanced NR");
        backend.inspectMotion();
        if (method==1) {
            settings.amdQuality=1-quality;
            require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Same-frame flow reconfiguration failed");
            backend.inspectMotion();
            key.cycle++; now+=16666667;
            require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"New AMD reference failed");
            require(!backend.diagnostics().flowValid,"Fresh AMD context exposed motion before its reference frame");
            backend.inspectMotion();
            key.cycle++; now+=16666667;
            require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"AMD motion after reference failed");
            require(backend.diagnostics().flowValid,"AMD motion did not become valid after reference");
            backend.inspectMotion();
        }
        std::cout << "Flow directions, pixel units, GPU-only densification and duplicate frame checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
