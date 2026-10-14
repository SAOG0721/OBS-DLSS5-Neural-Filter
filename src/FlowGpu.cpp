// SPDX-License-Identifier: GPL-3.0-only
// FidelityFX setup adapted from Jay's MIT Resolve AmdFlow integration.
#include "FlowGpu.h"
#include <Windows.h>
#include <ffx_dx12.h>
#include <ffx_opticalflow.h>
#include <nvOpticalFlowD3D11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>
#include <cstdio>

namespace neural {
using Microsoft::WRL::ComPtr;
namespace {
void trace(const char* phase) {
    if (GetEnvironmentVariableW(L"NEURAL_NVOF_TRACE",nullptr,0)) {
        std::fprintf(stderr,"NVOF phase: %s\n",phase); std::fflush(stderr);
    }
}
void hr(HRESULT result, const char* label) {
    if (FAILED(result)) throw std::runtime_error(std::string(label)+" HRESULT="+std::to_string(static_cast<unsigned>(result)));
}
void ffx(FfxErrorCode result, const char* label) {
    if (result != FFX_OK) throw std::runtime_error(std::string(label)+" FFX="+std::to_string(result));
}
void nv(NV_OF_STATUS result, const char* label) {
    if (result != NV_OF_SUCCESS) throw std::runtime_error(std::string(label)+" NVOF="+std::to_string(result));
}
ComPtr<ID3D12Resource> texture(ID3D12Device* device, Extent e, DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=e.width; desc.Height=e.height; desc.DepthOrArraySize=1; desc.MipLevels=1;
    desc.SampleDesc.Count=1; desc.Format=format; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
        nullptr,IID_PPV_ARGS(&result)),"Allocate motion texture");
    return result;
}
}
struct FlowGpu::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> motion, scd;
    FfxOpticalflowContext context{};
    std::vector<unsigned char> scratch;
    bool amd=false;
    bool needsReference=true;
    int selected=0, grid=8, quality=0;
    Extent color{}, vectors{};
    HMODULE module=nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> completed11,ready11;
    ID3D12Fence* registeredReady=nullptr;
    NvOFHandle handle=nullptr;
    ComPtr<ID3D12Fence> completed;
    std::uint64_t fenceValue=0;
    std::array<NvOFGPUBufferHandle,3> registration{};
    std::array<ID3D12Resource*,3> registeredResources{};
    std::array<ComPtr<ID3D11Texture2D>,3> shared11;
    ~Impl() {
        if (amd) ffxOpticalflowContextDestroy(&context);
        if (handle) {
            trace("destructor NVOF enter");
            for (auto h : registration) if (h) {
                api.nvOFUnregisterResourceD3D11(h);
                trace("destructor unregister complete");
            }
            api.nvOFDestroy(handle);
            trace("destructor NVOF complete");
        }
        if (module) FreeLibrary(module);
    }
    void registerResource(int index, ID3D12Resource* resource, ID3D11Texture2D* provided=nullptr) {
        trace("registerResource enter");
        if (registeredResources[index] == resource) return;
        if (registration[index]) {
            nv(api.nvOFUnregisterResourceD3D11(registration[index]),"Unregister NVOF texture");
            registration[index]=nullptr;
        }
        if (provided) {
            ComPtr<IDXGIResource1> share;
            hr(provided->QueryInterface(IID_PPV_ARGS(&share)),"Query NVOF input sharing");
            HANDLE token=nullptr;
            hr(share->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ,nullptr,&token),"Share NVOF input texture");
            shared11[index].Reset();
            const auto opened=device11->OpenSharedResource1(token,IID_PPV_ARGS(&shared11[index]));
            CloseHandle(token); hr(opened,"Open NVOF input on private context");
        }
        if (!shared11[index]) throw std::runtime_error("NVOF requires a same-adapter shared D3D11 texture");
        nv(api.nvOFRegisterResourceD3D11(handle,shared11[index].Get(),&registration[index]),"Register NVOF D3D11 texture");
        trace("registerResource complete");
        registeredResources[index]=resource;
    }
};
FlowGpu::FlowGpu() : impl_(std::make_unique<Impl>()) {}
FlowGpu::~FlowGpu() = default;
void FlowGpu::initialize(ID3D12Device* device,ID3D11Device* obsDevice,Extent inference,const Settings& settings) {
    auto next=std::make_unique<Impl>();
    auto& p=*next; p.device=device;
    p.selected=settings.flow ? settings.flow : (settings.af >= 2 ? 1 : 0);
    if (!p.selected) { impl_=std::move(next); return; }
    if (p.selected == 1) {
        p.quality=settings.flow == 0 ? 1 : settings.amdQuality;
        p.color=p.quality ? inference : Extent{std::max(1u,inference.width/2),std::max(1u,inference.height/2)};
        p.scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
        FfxOpticalflowContextDescription d{}; d.resolution={p.color.width,p.color.height};
        ffx(ffxGetInterfaceDX12(&d.backendInterface,ffxGetDeviceDX12(device),p.scratch.data(),
            p.scratch.size(),FFX_OPTICALFLOW_CONTEXT_COUNT),"Initialize AMDOF backend");
        ffx(ffxOpticalflowContextCreate(&p.context,&d),"Create AMDOF context");
        p.amd=true;
        FfxOpticalflowSharedResourceDescriptions shared{};
        ffx(ffxOpticalflowGetSharedResourceDescriptions(&p.context,&shared),"Query AMDOF outputs");
        p.vectors={shared.opticalFlowVector.resourceDescription.width,shared.opticalFlowVector.resourceDescription.height};
        if (shared.opticalFlowVector.resourceDescription.format != FFX_API_SURFACE_FORMAT_R16G16_SINT)
            throw std::runtime_error("AMDOF vector format is unsupported");
        p.motion=texture(device,p.vectors,DXGI_FORMAT_R16G16_SINT);
        const auto& sd=shared.opticalFlowSCD.resourceDescription;
        p.scd=texture(device,{sd.width,sd.height},DXGI_FORMAT_R32_UINT);
    } else {
        p.quality=settings.nvQuality;
        p.color=inference; p.grid=p.quality >= 4 ? 2 : 4;
        p.vectors={(inference.width+p.grid-1)/p.grid,(inference.height+p.grid-1)/p.grid};
        p.module=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!p.module) throw std::runtime_error("NVOF driver component is unavailable");
        using CreateApi=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*);
        const auto create=reinterpret_cast<CreateApi>(GetProcAddress(p.module,"NvOFAPICreateInstanceD3D11"));
        if (!create) throw std::runtime_error("NVOF D3D11 API is unavailable");
        nv(create(NV_OF_API_VERSION,&p.api),"Query NVOF API");
        if (!p.api.nvCreateOpticalFlowD3D11 || !p.api.nvOFInit || !p.api.nvOFExecute ||
            !p.api.nvOFRegisterResourceD3D11 || !p.api.nvOFUnregisterResourceD3D11 || !p.api.nvOFDestroy)
            throw std::runtime_error("NVOF required interfaces are missing");
        // Optical flow owns its D3D11 immediate context. Its wait for D3D12
        // preparation must not stall the OBS rendering/encoding context.
        ComPtr<IDXGIDevice> dxgi; hr(obsDevice->QueryInterface(IID_PPV_ARGS(&dxgi)),"Read NVOF adapter");
        ComPtr<IDXGIAdapter> adapter; hr(dxgi->GetAdapter(&adapter),"Read NVOF rendering adapter");
        ComPtr<ID3D11Device> privateDevice;
        ComPtr<ID3D11DeviceContext> context;
        hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&privateDevice,nullptr,&context),"Create private NVOF D3D11 device");
        hr(privateDevice.As(&p.device11),"NVOF shared-fence device capability");
        hr(context.As(&p.context11),"NVOF shared-fence context capability");
        nv(p.api.nvCreateOpticalFlowD3D11(privateDevice.Get(),context.Get(),&p.handle),"Create NVOF D3D11 instance");
        uint32_t count=0;
        nv(p.api.nvOFGetSurfaceFormatCountD3D11(p.handle,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,&count),"Query NVOF formats");
        std::vector<DXGI_FORMAT> formats(count);
        nv(p.api.nvOFGetSurfaceFormatD3D11(p.handle,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,formats.data()),"Read NVOF formats");
        if (std::find(formats.begin(),formats.end(),DXGI_FORMAT_R8_UNORM) == formats.end())
            throw std::runtime_error("NVOF lacks the required R8 grayscale input format");
        NV_OF_INIT_PARAMS init{}; init.width=inference.width; init.height=inference.height;
        init.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(p.grid);
        init.mode=NV_OF_MODE_OPTICALFLOW;
        init.perfLevel=p.quality == 1 ? NV_OF_PERF_LEVEL_FAST :
                       (p.quality == 3 || p.quality == 5 ? NV_OF_PERF_LEVEL_SLOW : NV_OF_PERF_LEVEL_MEDIUM);
        init.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        trace("nvOFInit enter");
        nv(p.api.nvOFInit(p.handle,&init),"Initialize NVOF");
        trace("nvOFInit complete");
        D3D11_TEXTURE2D_DESC vectors{};
        vectors.Width=p.vectors.width; vectors.Height=p.vectors.height;
        vectors.MipLevels=1; vectors.ArraySize=1; vectors.Format=DXGI_FORMAT_R16G16_SINT;
        vectors.SampleDesc.Count=1; vectors.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        vectors.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        hr(p.device11->CreateTexture2D(&vectors,nullptr,&p.shared11[2]),"Create NVOF shared vector texture");
        ComPtr<IDXGIResource1> vectorDxgi;
        hr(p.shared11[2].As(&vectorDxgi),"Query NVOF shared vector resource");
        HANDLE vectorHandle=nullptr;
        hr(vectorDxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&vectorHandle),"Share NVOF vectors");
        const auto vectorResult=device->OpenSharedHandle(vectorHandle,IID_PPV_ARGS(&p.motion));
        CloseHandle(vectorHandle); hr(vectorResult,"Open NVOF vectors in D3D12");
        trace("output texture created");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&p.completed)),"Create NVOF fence");
        HANDLE shared=nullptr;
        hr(device->CreateSharedHandle(p.completed.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"Share NVOF completion fence");
        const auto result=p.device11->OpenSharedFence(shared,IID_PPV_ARGS(&p.completed11));
        CloseHandle(shared); hr(result,"Open NVOF completion fence");
        p.registerResource(2,p.motion.Get());
    }
    impl_=std::move(next);
}
void FlowGpu::recordAmd(ID3D12GraphicsCommandList* list,ID3D12Resource* color,bool reset) {
    auto& p=*impl_; if (p.selected != 1) return;
    reset=reset || p.needsReference;
    FfxOpticalflowDispatchDescription d{}; d.commandList=ffxGetCommandListDX12(list); d.reset=reset;
    d.color=ffxGetResourceDX12(color,ffxGetResourceDescriptionDX12(color),L"Neural filter color",FFX_API_RESOURCE_STATE_COMMON);
    d.opticalFlowVector=ffxGetResourceDX12(p.motion.Get(),
        ffxGetResourceDescriptionDX12(p.motion.Get(),FFX_API_RESOURCE_USAGE_UAV),L"Neural filter vectors",FFX_API_RESOURCE_STATE_COMMON);
    d.opticalFlowSCD=ffxGetResourceDX12(p.scd.Get(),
        ffxGetResourceDescriptionDX12(p.scd.Get(),FFX_API_RESOURCE_USAGE_UAV),L"Neural filter scene change",FFX_API_RESOURCE_STATE_COMMON);
    d.backbufferTransferFunction=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB; d.minMaxLuminance={0,1};
    ffx(ffxOpticalflowContextDispatch(&p.context,&d),"Dispatch AMDOF");
    if (reset) {
        // The SDK suppresses flow for its first six frame indices. Prime the
        // same reference on the GPU; this does not accept additional OBS frames.
        d.reset=false;
        for (int i=0;i<5;++i) ffx(ffxOpticalflowContextDispatch(&p.context,&d),"Prime AMDOF reference");
    }
    p.needsReference=false;
}
void FlowGpu::submitNv(ID3D12Resource* current,ID3D12Resource* previous,
                       ID3D11Texture2D* current11,ID3D11Texture2D* previous11,
                       ID3D12Fence* ready,std::uint64_t readyValue,bool reset) {
    auto& p=*impl_; if (p.selected != 2) return;
    trace("submitNv enter");
    p.registerResource(0,current,current11); p.registerResource(1,previous,previous11);
    if (p.registeredReady!=ready) {
        HANDLE shared=nullptr;
        hr(p.device->CreateSharedHandle(ready,nullptr,GENERIC_ALL,nullptr,&shared),"Share NVOF input fence");
        const auto result=p.device11->OpenSharedFence(shared,IID_PPV_ARGS(&p.ready11));
        CloseHandle(shared); hr(result,"Open NVOF input fence");
        p.registeredReady=ready;
    }
    hr(p.context11->Wait(p.ready11.Get(),readyValue),"NVOF wait for GPU grayscale conversion");
    NV_OF_EXECUTE_INPUT_PARAMS in{};
    in.inputFrame=p.registration[0]; in.referenceFrame=p.registration[1];
    in.disableTemporalHints=(reset || p.needsReference) ? NV_OF_TRUE : NV_OF_FALSE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out{}; out.outputBuffer=p.registration[2];
    nv(p.api.nvOFExecute(p.handle,&in,&out),"Dispatch NVOF D3D11");
    hr(p.context11->Signal(p.completed11.Get(),++p.fenceValue),"Signal NVOF completion");
    p.context11->Flush();
    p.needsReference=false;
    trace("submitNv complete");
}
ID3D12Resource* FlowGpu::vectors() const noexcept { return impl_->motion.Get(); }
ID3D12Fence* FlowGpu::fence() const noexcept { return impl_->completed.Get(); }
std::uint64_t FlowGpu::fenceValue() const noexcept { return impl_->fenceValue; }
Extent FlowGpu::vectorExtent() const noexcept { return impl_->vectors; }
Extent FlowGpu::colorExtent() const noexcept { return impl_->color; }
DXGI_FORMAT FlowGpu::colorFormat() const noexcept { return impl_->selected == 2 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; }
int FlowGpu::grid() const noexcept { return impl_->grid; }
float FlowGpu::divisor() const noexcept { return impl_->selected == 2 ? 32.f : 1.f; }
int FlowGpu::method() const noexcept { return impl_->selected; }
bool FlowGpu::readyForMotion() const noexcept { return impl_->selected==2 || !impl_->needsReference; }
std::string FlowGpu::description() const {
    const auto& p=*impl_;
    if (!p.selected) return "none";
    return std::string(p.selected == 1 ? "AMDOF" : "NVOF")+
        "; quality="+std::to_string(p.quality)+"; grid="+std::to_string(p.grid)+
        "; current-to-previous/inference-pixels/y-down; divisor="+std::to_string(divisor())+
        (p.selected == 2 ? "; api=D3D11; input=R8-grayscale; GPU-shared-fences" : "; api=D3D12; input=RGBA8-sRGB");
}
}
