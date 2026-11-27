// SPDX-License-Identifier: GPL-3.0-only
#include "ShaderSource.h"
#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
namespace {
struct Pixel { float r=0,g=0,b=0,a=1; };
struct UInt2 { unsigned x=0,y=0; };
struct alignas(16) Parameters {
    UInt2 source, inference, flow, flowColor;
    unsigned flowGrid=8; float flowDivisor=1; unsigned stageMode=0,view=0;
    float mix=1,strength=1,chroma=1,lightness=1,darken=1,brighten=1,hue=0,darkProtect=0;
    float brightProtect=0,compression=0,low=1,high=1;
    unsigned debug=0,af=0; float weight=0,appearWeight=0;
    unsigned history=0,motion=0; float testDelta=0; unsigned recoveryFlags=0;
    float midtone=1,chromaProtection=0; unsigned residualReuse=0,residualAge=0;
};
static_assert(sizeof(Parameters)==144);
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
void hr(HRESULT value,const char* message) { require(SUCCEEDED(value),message); }
float distance(Pixel a,Pixel b) {
    return std::sqrt((a.r-b.r)*(a.r-b.r)+(a.g-b.g)*(a.g-b.g)+(a.b-b.b)*(a.b-b.b));
}
float decode(float c) { return c<=.04045f ? c/12.92f : std::pow((c+.055f)/1.055f,2.4f); }
float encode(float c) { return c<=.0031308f ? c*12.92f : 1.055f*std::pow(c,1/2.4f)-.055f; }
Pixel encoded(Pixel p) { return {encode(p.r),encode(p.g),encode(p.b),p.a}; }
Pixel decoded(Pixel p) { return {decode(p.r),decode(p.g),decode(p.b),p.a}; }
Pixel lab(Pixel c) {
    const auto l=std::cbrt(.4122214708f*c.r+.5363325363f*c.g+.0514459929f*c.b);
    const auto m=std::cbrt(.2119034982f*c.r+.6806995451f*c.g+.1073969566f*c.b);
    const auto s=std::cbrt(.0883024619f*c.r+.2817188376f*c.g+.6299787005f*c.b);
    return {.2104542553f*l+.793617785f*m-.0040720468f*s,
        1.9779984951f*l-2.428592205f*m+.4505937099f*s,
        .0259040371f*l+.7827717662f*m-.808675766f*s,1};
}
Pixel add(Pixel a,Pixel b) { return {a.r+b.r,a.g+b.g,a.b+b.b,a.a}; }
struct Texture {
    ComPtr<ID3D11Texture2D> resource;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    unsigned width=0,height=0;
    DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT;
};
class Gpu {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    std::array<ComPtr<ID3D11ComputeShader>,5> shaders;
    enum Entry { Proxy,Delta,Temporal,Compose,Reuse };
    Gpu() {
        ComPtr<IDXGIFactory1> factory;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create shader-test factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (unsigned i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId==0x10de) break;
            adapter.Reset();
        }
        require(adapter!=nullptr,"Native NVIDIA GPU is required for shader contracts");
        hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create shader-test device");
        const char* entries[]{"Proxy","Delta","Temporal","Compose","Reuse"};
        for (unsigned i=0;i<shaders.size();++i) {
            ComPtr<ID3DBlob> code,error;
            const auto compiled=D3DCompile(neural::shaderSource,sizeof(neural::shaderSource)-1,"neural.hlsl",nullptr,
                nullptr,entries[i],"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
            require(SUCCEEDED(compiled),std::string("Compile ")+entries[i]+": "+
                (error ? std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()) : ""));
            hr(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i]),"Create contract shader");
            if (i==Reuse) {
                ComPtr<ID3D11ShaderReflection> reflection;
                hr(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection)),"Reflect shader ABI");
                auto* buffer=reflection->GetConstantBufferByName("Parameters");
                D3D11_SHADER_BUFFER_DESC desc{}; hr(buffer->GetDesc(&desc),"Read shader constants ABI");
                require(desc.Size==sizeof(Parameters),"Shader constants size differs from the C++ ABI");
                for (const auto& field : std::array<std::pair<const char*,unsigned>,4>{{
                    {"midtone",128},{"chromaProtection",132},{"residualReuse",136},{"residualAge",140}}}) {
                    D3D11_SHADER_VARIABLE_DESC variable{};
                    hr(buffer->GetVariableByName(field.first)->GetDesc(&variable),"Read shader field ABI");
                    require(variable.StartOffset==field.second,std::string("Shader offset mismatch: ")+field.first);
                }
            }
        }
        D3D11_BUFFER_DESC b{}; b.ByteWidth=sizeof(Parameters); b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        hr(device->CreateBuffer(&b,nullptr,&constants),"Create shader constants");
        D3D11_SAMPLER_DESC s{}; s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD=D3D11_FLOAT32_MAX;
        hr(device->CreateSamplerState(&s,&sampler),"Create shader sampler");
    }
    Texture texture(unsigned width,unsigned height,const std::vector<Pixel>& pixels={},
                    DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT) {
        Texture result; result.width=width; result.height=height; result.format=format;
        D3D11_TEXTURE2D_DESC d{}; d.Width=width; d.Height=height; d.MipLevels=1; d.ArraySize=1;
        d.SampleDesc.Count=1; d.Format=format; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        hr(device->CreateTexture2D(&d,nullptr,&result.resource),"Create shader texture");
        hr(device->CreateShaderResourceView(result.resource.Get(),nullptr,&result.srv),"Create shader SRV");
        hr(device->CreateUnorderedAccessView(result.resource.Get(),nullptr,&result.uav),"Create shader UAV");
        if (!pixels.empty()) upload(result,pixels);
        return result;
    }
    void upload(Texture& texture,const std::vector<Pixel>& pixels) {
        require(pixels.size()==static_cast<size_t>(texture.width)*texture.height,"Wrong shader upload size");
        if (texture.format==DXGI_FORMAT_R16G16B16A16_FLOAT) {
            std::vector<HALF> half(pixels.size()*4);
            for (size_t i=0;i<pixels.size();++i) {
                half[i*4]=XMConvertFloatToHalf(pixels[i].r); half[i*4+1]=XMConvertFloatToHalf(pixels[i].g);
                half[i*4+2]=XMConvertFloatToHalf(pixels[i].b); half[i*4+3]=XMConvertFloatToHalf(pixels[i].a);
            }
            context->UpdateSubresource(texture.resource.Get(),0,nullptr,half.data(),texture.width*8,0);
        } else if (texture.format==DXGI_FORMAT_R8G8B8A8_UNORM) {
            std::vector<unsigned char> bytes(pixels.size()*4);
            for (size_t i=0;i<pixels.size();++i) {
                const float values[]{pixels[i].r,pixels[i].g,pixels[i].b,pixels[i].a};
                for (unsigned c=0;c<4;++c) bytes[i*4+c]=static_cast<unsigned char>(std::round(std::clamp(values[c],0.f,1.f)*255));
            }
            context->UpdateSubresource(texture.resource.Get(),0,nullptr,bytes.data(),texture.width*4,0);
        } else {
            require(texture.format==DXGI_FORMAT_R32G32B32A32_FLOAT,"Unsupported contract texture format");
            context->UpdateSubresource(texture.resource.Get(),0,nullptr,pixels.data(),texture.width*sizeof(Pixel),0);
        }
    }
    std::vector<Pixel> read(const Texture& texture) {
        auto d=D3D11_TEXTURE2D_DESC{}; texture.resource->GetDesc(&d);
        d.BindFlags=0; d.Usage=D3D11_USAGE_STAGING; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; hr(device->CreateTexture2D(&d,nullptr,&staging),"Create shader readback");
        context->CopyResource(staging.Get(),texture.resource.Get()); context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{}; HRESULT result;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do {
            result=context->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if (result==DXGI_ERROR_WAS_STILL_DRAWING) Sleep(1);
        } while (result==DXGI_ERROR_WAS_STILL_DRAWING && std::chrono::steady_clock::now()<deadline);
        hr(result,"Bounded shader readback");
        std::vector<Pixel> pixels(static_cast<size_t>(d.Width)*d.Height);
        for (unsigned y=0;y<d.Height;++y) {
            const auto* row=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
            for (unsigned x=0;x<d.Width;++x) {
                auto& p=pixels[static_cast<size_t>(y)*d.Width+x];
                if (d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const auto* v=reinterpret_cast<const HALF*>(row)+x*4;
                    p={XMConvertHalfToFloat(v[0]),XMConvertHalfToFloat(v[1]),XMConvertHalfToFloat(v[2]),XMConvertHalfToFloat(v[3])};
                } else if (d.Format==DXGI_FORMAT_R8G8B8A8_UNORM) {
                    const auto* v=row+x*4; p={v[0]/255.f,v[1]/255.f,v[2]/255.f,v[3]/255.f};
                } else p=reinterpret_cast<const Pixel*>(row)[x];
            }
        }
        context->Unmap(staging.Get(),0); return pixels;
    }
    void run(Entry entry,Parameters params,UInt2 extent,const std::array<Texture*,6>& inputs,
             const std::array<Texture*,2>& outputs) {
        std::array<ID3D11ShaderResourceView*,6> srvs{};
        std::array<ID3D11UnorderedAccessView*,2> uavs{};
        for (unsigned i=0;i<srvs.size();++i) if (inputs[i]) srvs[i]=inputs[i]->srv.Get();
        for (unsigned i=0;i<uavs.size();++i) if (outputs[i]) uavs[i]=outputs[i]->uav.Get();
        context->UpdateSubresource(constants.Get(),0,nullptr,&params,0,0);
        context->CSSetShader(shaders[entry].Get(),nullptr,0);
        context->CSSetConstantBuffers(0,1,constants.GetAddressOf());
        context->CSSetSamplers(0,1,sampler.GetAddressOf());
        context->CSSetShaderResources(0,static_cast<UINT>(srvs.size()),srvs.data());
        context->CSSetUnorderedAccessViews(0,static_cast<UINT>(uavs.size()),uavs.data(),nullptr);
        context->Dispatch((extent.x+7)/8,(extent.y+7)/8,1);
        srvs.fill(nullptr); uavs.fill(nullptr);
        context->CSSetShaderResources(0,static_cast<UINT>(srvs.size()),srvs.data());
        context->CSSetUnorderedAccessViews(0,static_cast<UINT>(uavs.size()),uavs.data(),nullptr);
    }
};
constexpr unsigned width=16,height=8;
std::vector<Pixel> fill(Pixel p) { return std::vector<Pixel>(width*height,p); }
Parameters parameters() { Parameters p; p.source=p.inference={width,height}; return p; }
void equalPixels(const std::vector<Pixel>& a,const std::vector<Pixel>& b,const char* message) {
    require(a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(Pixel))==0,message);
}

void directComposeContracts(Gpu& gpu) {
    auto p=parameters(); p.af=0; p.history=1; p.motion=1; p.weight=.95f;
    // Non-power-of-two extents exercise the UV/sample-center arithmetic used
    // by Temporal instead of making SampleLevel trivially equivalent to Load.
    constexpr unsigned directWidth=257,directHeight=145;
    p.source=p.inference={directWidth,directHeight};
    auto filled=[](Pixel value) { return std::vector<Pixel>(directWidth*directHeight,value); };
    std::vector<Pixel> pixels(directWidth*directHeight),corrections(pixels.size());
    const float alphas[]{0,.001f,.25f,.5f,1};
    const Pixel residuals[]{{0,0,0,1},{.015f,0,-.01f,1},{-.03f,.02f,.01f,.25f},
        {1.2f,-.4f,.6f,1},{0,0,0,0}};
    for (size_t i=0;i<pixels.size();++i) {
        const float alpha=alphas[i%std::size(alphas)];
        const float red=i%11==0 ? -.05f : .2f+static_cast<float>(i%7)*.015f;
        pixels[i]={red*alpha,.3f*alpha,.12f*alpha,alpha};
        corrections[i]=residuals[(i/5)%std::size(residuals)];
    }
    auto source=gpu.texture(directWidth,directHeight,pixels),delta=gpu.texture(directWidth,directHeight,corrections);
    auto previous=gpu.texture(directWidth,directHeight,filled({-.1f,.15f,.05f,1}));
    auto metrics=gpu.texture(directWidth,directHeight,filled({.01f,.35f,.7f,1}));
    auto stable=gpu.texture(directWidth,directHeight),referenceOutput=gpu.texture(directWidth,directHeight),
        directOutput=gpu.texture(directWidth,directHeight);
    auto compare=[&](const char* message) {
        gpu.run(Gpu::Temporal,p,p.source,{&delta,&previous,&source,&source},{&stable});
        gpu.run(Gpu::Compose,p,p.source,{&source,&stable,&metrics},{&referenceOutput});
        gpu.run(Gpu::Compose,p,p.source,{&source,&delta,&metrics},{&directOutput});
        equalPixels(gpu.read(directOutput),gpu.read(referenceOutput),message);
    };
    for (unsigned view : {0u,2u}) for (float mix : {0.f,.375f,1.f}) {
        p.view=view; p.mix=mix;
        compare("Same-size AF-off direct composition differs from Temporal -> Compose");
    }
    // Compose duplicates Temporal's sanitizing guard so finite and invalid
    // observations also keep the same diagnostic and transparent-edge output.
    p.view=0; p.mix=1;
    for (unsigned debug=1;debug<=7;++debug) {
        p.debug=debug;
        compare("Finite AF-off diagnostic differs without the identity Temporal pass");
    }
    p.debug=0; p.view=3;
    compare("Finite correction view differs without the identity Temporal pass");
    p.view=0;
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        gpu.upload(delta,filled({invalid,.02f,-.01f,1}));
        for (unsigned view : {0u,2u}) {
            p.view=view;
            compare("AF-off direct composition lost the normal-output finite guard");
            equalPixels(gpu.read(directOutput),gpu.read(source),"Non-finite correction did not preserve the original");
        }
        p.view=0;
        for (unsigned debug : {1u,4u,6u,7u}) {
            p.debug=debug;
            compare("AF-off direct composition lost the diagnostic finite guard");
        }
        p.debug=0; p.view=3;
        compare("AF-off direct correction view lost the finite guard");
        p.view=0;
    }
    for (float invalidValidity : {0.f,std::numeric_limits<float>::quiet_NaN()}) {
        gpu.upload(delta,filled({.02f,-.01f,.03f,invalidValidity}));
        compare("AF-off direct composition ignored residual validity");
        equalPixels(gpu.read(directOutput),gpu.read(source),"Invalid residual validity changed the original");
    }
    // Exercise the actual invalid-reuse producer contract. A rejected residual
    // has both RGB and validity zero, so direct composition cannot revive it.
    auto reference=gpu.texture(directWidth,directHeight,filled({.8f,.7f,.6f,1}));
    auto historical=gpu.texture(directWidth,directHeight,filled({.02f,-.01f,.03f,1}));
    p=parameters(); p.residualReuse=1; p.residualAge=1;
    p.source=p.inference={directWidth,directHeight};
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&historical,&reference},{&delta});
    equalPixels(gpu.read(delta),filled({0,0,0,0}),"Rejected reuse violated the direct-composition observation contract");
    compare("AF-off direct composition resurrected a rejected reuse correction");
    equalPixels(gpu.read(directOutput),gpu.read(source),"Rejected reuse changed the current source or alpha");
    std::cout << "Shader direct composition: same-size AF-off normal/finite diagnostics, mix, alpha, signed gamut, non-finite guard and rejected reuse passed\n";
}

void colorContracts(Gpu& gpu) {
    auto p=parameters();
    auto sourcePixels=fill({.2f,.15f,.1f,.5f});
    for (unsigned x=0;x<width;++x) {
        const float alpha=(x%4)*.25f;
        for (unsigned y=0;y<height;++y) sourcePixels[y*width+x]={(-.1f+x*.07f)*alpha,.3f*alpha,.15f*alpha,alpha};
    }
    auto source=gpu.texture(width,height,sourcePixels),proxy=gpu.texture(width,height,{},DXGI_FORMAT_R8G8B8A8_UNORM);
    auto delta=gpu.texture(width,height),metrics=gpu.texture(width,height),stable=gpu.texture(width,height),output=gpu.texture(width,height);
    gpu.run(Gpu::Proxy,p,p.inference,{&source},{&proxy});
    p.midtone=1.8f; p.chromaProtection=.8f; p.darkProtect=.6f; p.low=.3f; p.high=1.7f;
    gpu.run(Gpu::Delta,p,p.inference,{&proxy,&proxy},{&delta,&metrics});
    gpu.run(Gpu::Temporal,p,p.source,{&delta,nullptr,&source},{&stable});
    gpu.run(Gpu::Compose,p,p.source,{&source,&stable,&metrics},{&output});
    equalPixels(gpu.read(output),gpu.read(source),"Non-default controls changed exact zero-residual proxy identity");
    gpu.upload(stable,fill({4,-3,2,1})); p.mix=0;
    gpu.run(Gpu::Compose,p,p.source,{&source,&stable,&metrics},{&output});
    equalPixels(gpu.read(output),gpu.read(source),"Zero output mix changed original RGB/alpha");

    auto base=gpu.texture(width,height,fill(encoded({.18f,.18f,.18f,1})),DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto model=gpu.texture(width,height,fill(encoded({.24f,.24f,.24f,1})),DXGI_FORMAT_R32G32B32A32_FLOAT);
    p=parameters(); p.midtone=0;
    gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto zero=gpu.read(delta)[width*3+4];
    require(distance(zero,{0,0,0,1})<.0002f,"Midtone zero left a midtone lightness correction");
    p.midtone=1; gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto normal=gpu.read(delta)[width*3+4];
    p.midtone=2; gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto strong=gpu.read(delta)[width*3+4];
    const Pixel gray{.18f,.18f,.18f,1};
    const auto normalL=lab(add(gray,normal)).r-lab(gray).r;
    const auto strongL=lab(add(gray,strong)).r-lab(gray).r;
    require(normalL>.02f && std::abs(strongL-2*normalL)<.0005f,"Midtone strength did not independently scale Oklab L");

    const Pixel colorBase{.25f,.1f,.04f,1};
    gpu.upload(base,fill(encoded(colorBase))); gpu.upload(model,fill(encoded({.13f,.2f,.06f,1})));
    p=parameters(); p.chromaProtection=1;
    gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto protectedLab=lab(add(colorBase,gpu.read(delta)[width*3+4]));
    const auto baseLab=lab(colorBase);
    require(std::hypot(protectedLab.g-baseLab.g,protectedLab.b-baseLab.b)<.0004f,
        "Global chroma protection failed to suppress both Oklab a/b deltas");
    require(std::abs(protectedLab.r-baseLab.r)>.005f,"Global chroma protection suppressed lightness too");

    // An equivalent candidate constructed from the shader's own raw bands must
    // receive the same nonlinear controls. This detects controls-before-bands.
    std::vector<Pixel> originals(width*height),models(width*height);
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const float f=static_cast<float>((x+y)%3);
        originals[y*width+x]=encoded({.15f+f*.01f,.20f-f*.008f,.12f+f*.006f,1});
        models[y*width+x]=encoded({.16f+(x%2)*.08f,.18f+(y%2)*.06f,.13f+f*.015f,1});
    }
    gpu.upload(base,originals); gpu.upload(model,models);
    p=parameters(); p.low=.35f; p.high=1.45f;
    gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto bands=gpu.read(delta);
    std::vector<Pixel> equivalent(models.size());
    for (size_t i=0;i<equivalent.size();++i) equivalent[i]=encoded(add(decoded(originals[i]),bands[i]));
    auto reconstructed=gpu.texture(width,height,equivalent,DXGI_FORMAT_R32G32B32A32_FLOAT);
    p.strength=1.3f; p.midtone=1.6f; p.chroma=1.4f; p.hue=.3f; p.chromaProtection=.2f;
    p.darkProtect=.6f; p.brightProtect=.5f; p.compression=.7f;
    gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto combined=gpu.read(delta);
    p.low=p.high=1;
    gpu.run(Gpu::Delta,p,p.inference,{&base,&reconstructed},{&delta,&metrics});
    const auto sequential=gpu.read(delta);
    float maximumError=0;
    for (size_t i=0;i<combined.size();++i) maximumError=std::max(maximumError,distance(combined[i],sequential[i]));
    require(maximumError<.00025f,"Frequency bands were processed after nonlinear protection/controls");

    gpu.upload(base,fill(encoded({.2f,.1f,.03f,1})));
    gpu.upload(model,fill(encoded({.8f,.3f,.1f,1})));
    p=parameters(); p.strength=2; p.chroma=1.3f;
    gpu.run(Gpu::Delta,p,p.inference,{&base,&model},{&delta,&metrics});
    const auto unbounded=add({.2f,.1f,.03f,1},gpu.read(delta)[width*3+4]);
    require(unbounded.r>1 || unbounded.g<0 || unbounded.b<0,"Delta clipped a signed/out-of-gamut intermediate");

    // The lowered-inference path must interpolate the signed residual before
    // limiting color, and must compose against the full-size premultiplied base.
    auto largeSource=gpu.texture(width*2,height*2,std::vector<Pixel>(width*height*4,{.1f,.05f,.015f,.5f}));
    auto largeStable=gpu.texture(width*2,height*2),largeOutput=gpu.texture(width*2,height*2);
    p.source={width*2,height*2}; p.mix=.1f;
    gpu.run(Gpu::Temporal,p,p.source,{&delta,nullptr,&largeSource},{&largeStable});
    gpu.run(Gpu::Compose,p,p.source,{&largeSource,&largeStable,&metrics},{&largeOutput});
    const auto largeBasePixels=gpu.read(largeSource),largeDeltaPixels=gpu.read(largeStable),largePixels=gpu.read(largeOutput);
    for (size_t i=0;i<largePixels.size();++i) {
        const auto basePixel=largeBasePixels[i];
        const Pixel expected{basePixel.r+largeDeltaPixels[i].r*p.mix*basePixel.a,
            basePixel.g+largeDeltaPixels[i].g*p.mix*basePixel.a,
            basePixel.b+largeDeltaPixels[i].b*p.mix*basePixel.a,basePixel.a};
        require(distance(largePixels[i],expected)<.0003f && largePixels[i].a==basePixel.a,
            "Lowered-inference residual/alpha reconstruction failed before final mixing");
    }

    gpu.upload(source,fill({.1f,.15f,.05f,.5f})); gpu.upload(stable,fill({1.2f,-.4f,.6f,1}));
    p=parameters();
    gpu.run(Gpu::Compose,p,p.source,{&source,&stable,&metrics},{&output});
    const auto mapped=gpu.read(output)[width*3+4];
    require(mapped.a==.5f && mapped.r>=0 && mapped.g>=0 && mapped.b>=0 &&
        mapped.r<=.5f && mapped.g<=.5f && mapped.b<=.5f,"Final gamut map changed alpha or exceeded the output gamut");
    const Pixel straightMapped{mapped.r*2,mapped.g*2,mapped.b*2,1};
    const auto candidateLab=lab({1.4f,-.1f,.7f,1}),mappedLab=lab(straightMapped);
    require(std::abs(mappedLab.r-std::clamp(candidateLab.r,0.f,1.f))<.001f,"Final mapping changed Oklab lightness");
    const float cross=candidateLab.g*mappedLab.b-candidateLab.b*mappedLab.g;
    require(std::abs(cross)<.0003f && candidateLab.g*mappedLab.g+candidateLab.b*mappedLab.b>0,
        "Final mapping changed Oklab hue");
    require(distance(straightMapped,{1,0,.7f,1})>.05f,"Final mapping fell back to independent channel clipping");
    const float expectedScale=std::hypot(mappedLab.g,mappedLab.b)/std::hypot(candidateLab.g,candidateLab.b);
    p.debug=6; gpu.run(Gpu::Compose,p,p.source,{&source,&stable,&metrics},{&output});
    require(std::abs(gpu.read(output)[width*3+4].r*2-expectedScale)<.002f,
        "Gamut diagnostic did not report final-stage chroma scale");
    p.debug=0; p.mix=.1f;
    gpu.run(Gpu::Compose,p,p.source,{&source,&stable,&metrics},{&output});
    const auto mixed=gpu.read(output)[width*3+4];
    require(distance({mixed.r*2,mixed.g*2,mixed.b*2,1},{.32f,.26f,.16f,1})<.0007f,
        "Gamut mapping ran before output mixing");
    std::cout << "Shader color: exact proxy identity/mix/alpha, midtone, chroma protection, raw frequency order, signed residual and final gamut passed\n";
}

void temporalReuseContracts(Gpu& gpu) {
    auto p=parameters(); p.af=1; p.history=1; p.weight=.9f;
    auto current=gpu.texture(width,height,fill({.01f,0,-.02f,1}));
    auto history=gpu.texture(width,height,fill({-.01f,.1f,.01f,1}));
    auto source=gpu.texture(width,height,fill({.2f,.25f,.3f,1}));
    auto reference=gpu.texture(width,height,fill({.2f,.25f,.3f,1}));
    auto result=gpu.texture(width,height);
    gpu.run(Gpu::Temporal,p,p.source,{&current,&history,&source,&reference},{&result});
    require(gpu.read(result)[width*3+4].g==0,"Temporal history resurrected a zero correction component");
    for (unsigned af : {1u,2u,3u,4u}) {
        p.af=af; p.motion=1; p.debug=1;
        auto zeroMotion=gpu.texture(width,height,fill({0,0,0,1}));
        gpu.run(Gpu::Temporal,p,p.source,{&current,&history,&source,&reference,&zeroMotion},{&result});
        equalPixels(gpu.read(result),gpu.read(current),"Diagnostic view blended temporal history");
        p.debug=0; p.view=3;
        gpu.run(Gpu::Temporal,p,p.source,{&current,&history,&source,&reference,&zeroMotion},{&result});
        equalPixels(gpu.read(result),gpu.read(current),"Correction view blended temporal history");
        p.view=0;
        gpu.run(Gpu::Temporal,p,p.source,{&current,&history,&source,&reference,&zeroMotion},{&result});
        require(gpu.read(result)[width*3+4].g==0,"An AF mode resurrected a zero correction component");
        gpu.upload(current,fill({0,0,0,1}));
        gpu.run(Gpu::Temporal,p,p.source,{&current,&history,&source,&reference,&zeroMotion},{&result});
        equalPixels(gpu.read(result),gpu.read(current),"Zero current correction retained old temporal enhancement");
        gpu.upload(current,fill({.01f,0,-.02f,1}));
    }
    p=parameters(); p.residualReuse=1; p.residualAge=3;
    gpu.upload(history,fill({.02f,-.01f,.03f,1}));
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference},{&result});
    equalPixels(gpu.read(result),gpu.read(history),"In-place reuse changed a valid immutable residual");
    auto changedSource=gpu.read(source);
    for (auto& pixel:changedSource) pixel.r+=.005f;
    gpu.upload(source,changedSource);
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference},{&result});
    equalPixels(gpu.read(result),gpu.read(history),"Reuse subtracted an old NR output from the current original");
    p.residualAge=4;
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference},{&result});
    equalPixels(gpu.read(result),fill({0,0,0,0}),"Reuse allowed a source older than three input cycles");
    p.residualAge=1;
    gpu.upload(source,fill({.1f,.125f,.15f,.5f}));
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference},{&result});
    equalPixels(gpu.read(result),fill({0,0,0,0}),"Reuse accepted an incompatible opacity boundary");
    gpu.upload(source,fill({.7f,.2f,.1f,1}));
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference},{&result});
    equalPixels(gpu.read(result),fill({0,0,0,0}),"Reuse accepted a scene/color mismatch");
    auto stable=gpu.texture(width,height); p.af=1; p.history=1; p.weight=.95f;
    gpu.run(Gpu::Temporal,p,p.source,{&result,&history,&source,&reference},{&stable});
    equalPixels(gpu.read(stable),fill({0,0,0,0}),"Temporal resurrected enhancement rejected by reuse");

    gpu.upload(source,fill({.2f,.25f,.3f,1}));
    auto gradient=fill({0,0,0,1});
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) gradient[y*width+x]={.002f*x,-.001f*x,.003f*x,1};
    gpu.upload(history,gradient);
    auto motion=gpu.texture(width,height,fill({1,0,0,1}));
    p=parameters(); p.residualReuse=2; p.motion=1;
    const auto originalResidual=gpu.read(history);
    for (unsigned age=1;age<=3;++age) {
        p.residualAge=age;
        gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference,&motion,&motion,&motion},{&result});
        const auto reprojected=gpu.read(result);
        require(distance(reprojected[width*3+4],originalResidual[width*3+4+age])<.00001f &&
            reprojected[width*3+4].a==1,"Reuse failed newest-to-oldest backward motion composition");
        require(reprojected[width*3+width-age].a==0 && reprojected[width*3+width-age].r==0,
            "Reuse sampled beyond its source motion bounds");
    }
    p.motion=0;
    gpu.run(Gpu::Reuse,p,p.inference,{&source,&history,&reference,&motion,&motion,&motion},{&result});
    equalPixels(gpu.read(result),fill({0,0,0,0}),"Reuse accepted an unavailable motion chain");

    // Magpie 0.6.13 NVOF local history recovery contracts:
    // 1. Stationary textured block with bad motion recovers residual in Reuse.
    std::vector<Pixel> textured(width*height), shifted(width*height), residualPattern(width*height);
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const float r=.15f+((x%4)*.06f)+((y%3)*.04f);
        const float g=.20f+(((x+1)%4)*.05f);
        const float b=.25f+(((y+2)%4)*.04f);
        textured[y*width+x]={r,g,b,1};
        residualPattern[y*width+x]={.02f+.001f*x,-.01f,.015f,1};
    }
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const unsigned sx=std::clamp<int>(static_cast<int>(x)-2,0,static_cast<int>(width-1));
        shifted[y*width+x]=textured[y*width+sx];
    }
    auto texSrc=gpu.texture(width,height,textured);
    auto texRef=gpu.texture(width,height,textured);
    auto texRes=gpu.texture(width,height,residualPattern);
    auto badMotion=gpu.texture(width,height,fill({7,0,0,1}));
    p=parameters(); p.residualReuse=2; p.residualAge=1; p.motion=1; p.recoveryFlags=1;
    gpu.run(Gpu::Reuse,p,p.inference,{&texSrc,&texRes,&texRef,&badMotion,&badMotion,&badMotion},{&result});
    const auto recoveredStationary=gpu.read(result);
    require(distance(recoveredStationary[width*3+4],residualPattern[width*3+4])<.0001f &&
        recoveredStationary[width*3+4].a==1,"NVOF stationary 3x3 recovery failed in Reuse");

    // 2. Shifted textured block (+2 px) with bad motion recovers via 8x8 tile search.
    gpu.upload(texSrc,shifted);
    gpu.run(Gpu::Reuse,p,p.inference,{&texSrc,&texRes,&texRef,&badMotion,&badMotion,&badMotion},{&result});
    const auto recoveredShifted=gpu.read(result);
    require(distance(recoveredShifted[width*3+6],residualPattern[width*3+4])<.0001f &&
        recoveredShifted[width*3+6].a>.99f,"NVOF 8x8 local patch recovery failed for shifted block in Reuse");

    // 3. Alpha mismatch still rejects recovery.
    auto alphaMismatch=shifted;
    for (auto& px : alphaMismatch) { px.r*=.5f; px.g*=.5f; px.b*=.5f; px.a=.5f; }
    gpu.upload(texSrc,alphaMismatch);
    gpu.run(Gpu::Reuse,p,p.inference,{&texSrc,&texRes,&texRef,&badMotion,&badMotion,&badMotion},{&result});
    equalPixels(gpu.read(result),fill({0,0,0,0}),"NVOF local recovery bypassed alpha mismatch rejection");

    // 4. Flat untextured tile with bad motion must not accept arbitrary offsets.
    gpu.upload(texSrc,fill({.2f,.25f,.3f,1}));
    gpu.upload(texRef,fill({.7f,.2f,.1f,1}));
    gpu.run(Gpu::Reuse,p,p.inference,{&texSrc,&texRes,&texRef,&badMotion,&badMotion,&badMotion},{&result});
    equalPixels(gpu.read(result),fill({0,0,0,0}),"NVOF local recovery falsely accepted flat mismatch");

    // 5. Temporal NVOF recovery: shifted textured block with bad motion blends recovered history.
    gpu.upload(texSrc,shifted);
    gpu.upload(texRef,textured);
    auto curObs=gpu.texture(width,height,fill({.03f,.03f,.03f,1}));
    auto prevObs=gpu.texture(width,height,fill({.09f,.09f,.09f,1}));
    p=parameters(); p.af=2; p.history=1; p.motion=1; p.weight=.9f; p.recoveryFlags=0;
    gpu.run(Gpu::Temporal,p,p.source,{&curObs,&prevObs,&texSrc,&texRef,&badMotion},{&result});
    require(std::abs(gpu.read(result)[width*3+5].r-.03f)<.0001f,"Unrecovered bad motion unexpectedly blended history");
    p.recoveryFlags=1;
    gpu.run(Gpu::Temporal,p,p.source,{&curObs,&prevObs,&texSrc,&texRef,&badMotion},{&result});
    const auto temporalRecovered=gpu.read(result)[width*3+5];
    require(temporalRecovered.r>.04f,"NVOF Temporal patch recovery failed to blend shifted history");
    gpu.upload(curObs,fill({0,.03f,.03f,1}));
    gpu.run(Gpu::Temporal,p,p.source,{&curObs,&prevObs,&texSrc,&texRef,&badMotion},{&result});
    require(gpu.read(result)[width*3+5].r==0,"NVOF Temporal recovery resurrected a zero component");
    std::cout << "Shader temporal/reuse: diagnostics, zero components, immutable residual, source age, alpha/color rejection, three-motion chain, NVOF local recovery and validity passed\n";
}
}

int main() {
    try {
        Gpu gpu;
        directComposeContracts(gpu);
        colorContracts(gpu);
        temporalReuseContracts(gpu);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
