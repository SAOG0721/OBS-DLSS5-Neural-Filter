// SPDX-License-Identifier: GPL-3.0-only
#include "GpuBackend.h"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace neural;
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        Extent extent{512,288};
        ComPtr<IDXGIFactory1> factory; require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"Create GPU factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (unsigned i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (d.VendorId==0x10de && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
        require(adapter!=nullptr,"Native GPU unavailable");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Create temporal-test device");
        std::vector<unsigned short> source(static_cast<size_t>(extent.width)*extent.height*4);
        for (size_t i=0;i<source.size();i+=4) {
            source[i]=XMConvertFloatToHalf(.3f); source[i+1]=XMConvertFloatToHalf(.35f);
            source[i+2]=XMConvertFloatToHalf(.4f); source[i+3]=XMConvertFloatToHalf(1);
        }
        D3D11_TEXTURE2D_DESC d{}; d.Width=extent.width; d.Height=extent.height; d.ArraySize=1; d.MipLevels=1;
        d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=source.data(); data.SysMemPitch=extent.width*8;
        ComPtr<ID3D11Texture2D> input,output,staging;
        require(SUCCEEDED(device->CreateTexture2D(&d,&data,&input)) && SUCCEEDED(device->CreateTexture2D(&d,nullptr,&output)),"Create temporal-test textures");
        d.BindFlags=0; d.Usage=D3D11_USAGE_STAGING; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Create temporal probe");
        auto sample=[&]() {
            context->CopyResource(staging.Get(),output.Get()); context->Flush();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            HRESULT result;
            do { result=context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                if (result==DXGI_ERROR_WAS_STILL_DRAWING) Sleep(1);
            } while (result==DXGI_ERROR_WAS_STILL_DRAWING && std::chrono::steady_clock::now()<deadline);
            require(SUCCEEDED(result),"Bounded temporal readback");
            const auto* pixel=reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(mapped.pData)+
                (extent.height/2)*mapped.RowPitch)+(extent.width/2)*4;
            const float value=XMConvertHalfToFloat(pixel[0]);
            require(pixel[3]==XMConvertFloatToHalf(1),"Temporal mode changed alpha");
            context->Unmap(staging.Get(),0); return value;
        };
        double baseline=0;
        for (int mode=0;mode<5;++mode) {
            GpuBackend backend(BackendTestMode::ControlledDelta); Settings settings; settings.af=mode;
            std::vector<float> values;
            FrameKey key{1,0,1,extent}; std::uint64_t now=1000000000;
            for (int frame=0;frame<40;++frame) {
                require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,frame==0),backend.diagnostics().error);
                const auto value=sample(); if (frame>=10) values.push_back(value);
                const auto commits=backend.diagnostics().temporalCommits;
                require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Repeated temporal draw");
                require(backend.diagnostics().temporalCommits==commits,"Duplicate draw advanced AF history");
                sample();
                ++key.cycle; now+=16666667;
            }
            double mean=0,variance=0;
            for (auto v:values) mean+=v/values.size();
            for (auto v:values) variance+=(v-mean)*(v-mean)/values.size();
            if (!mode) baseline=variance;
            else require(variance<baseline*.8,"AF mode failed correction-variance reduction");
            require(!backend.diagnostics().nrMotionValid,"AF-only flow was incorrectly sent to NR");
            std::cout << "AF mode=" << mode << "; variance=" << variance << "; baseline=" << baseline
                      << "; flow=" << backend.diagnostics().flow << '\n';
            const auto resets=backend.diagnostics().resets;
            now+=300000000; ++key.cycle;
            require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Long-gap temporal reset failed");
            sample(); require(backend.diagnostics().resets>resets,"Long-gap history was reused");
            settings.low=settings.high=0; ++key.cycle; now+=16666667;
            require(backend.process(device.Get(),input.Get(),output.Get(),settings,key,now,false),"Zero frequency control failed");
            require(sample()==XMConvertHalfToFloat(source[(extent.height/2*extent.width+extent.width/2)*4]),"Zero frequency gains left a correction");
        }
        std::cout << "Five AF modes, variance, AF-only flow, duplicate commit, long-gap reset and frequency-zero contracts passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
