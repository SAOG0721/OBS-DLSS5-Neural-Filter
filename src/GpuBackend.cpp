// SPDX-License-Identifier: GPL-3.0-only
#include "GpuBackend.h"
#include "BackendPolicy.h"
#include "FenceWait.h"
#include <d3d12sdklayers.h>
#include "FlowGpu.h"
#include "NgxSession.h"
#include "ShaderSource.h"
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace neural {
using Microsoft::WRL::ComPtr;
namespace {
using Clock=std::chrono::steady_clock;
struct Busy : std::runtime_error { using std::runtime_error::runtime_error; };
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
void hr(HRESULT result,const char* phase) {
    if (FAILED(result)) {
        std::ostringstream message; message << phase << " HRESULT=0x" << std::hex << static_cast<unsigned>(result);
        throw std::runtime_error(message.str());
    }
}
struct alignas(16) Parameters {
    Extent source, inference, flow, flowColor;
    unsigned flowGrid=8; float flowDivisor=1; unsigned stageMode=0, view=0;
    float mix=1,strength=1,chroma=1,lightness=1,darken=1,brighten=1,hue=0,darkProtect=0;
    float brightProtect=0,compression=0,low=1,high=1;
    unsigned debug=0,af=0; float weight=0,appearWeight=0;
    unsigned history=0,motion=0; float testDelta=0; unsigned recoveryFlags=0;
    float midtone=1,chromaProtection=0; unsigned residualReuse=0,residualAge=0;
};
static_assert(sizeof(Parameters)==144);
enum Shader { Proxy, ModelTest, Delta, Dense, FlowColor, FlowLuma, Zero, Temporal, Compose, Reuse, ClearChanged, CompareInput, ShaderCount };
constexpr const char* entries[]{"Proxy","ModelTest","Delta","Dense","FlowColor","FlowLuma","Zero","Temporal","Compose","Reuse","ClearChanged","CompareInput"};
struct ShaderBytecode {
    std::array<std::vector<unsigned char>,ShaderCount> bytes;
    double elapsedMs=0;
};
const ShaderBytecode& shaderBytecode() {
    static const auto cached=[] {
        ShaderBytecode result; const auto start=Clock::now();
        for (unsigned i=0;i<ShaderCount;++i) {
            ComPtr<ID3DBlob> blob,error;
            const D3D_SHADER_MACRO verify[]={{"VERIFY_INPUT","1"},{nullptr,nullptr}};
            const auto compiled=D3DCompile(shaderSource,sizeof(shaderSource)-1,"neural.hlsl",i>=ClearChanged ? verify : nullptr,
                nullptr,entries[i],"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&error);
            if (FAILED(compiled)) throw std::runtime_error(std::string("Compile ")+entries[i]+": "+
                (error ? std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()) : "unknown shader error"));
            auto* data=static_cast<const unsigned char*>(blob->GetBufferPointer());
            result.bytes[i].assign(data,data+blob->GetBufferSize());
        }
        result.elapsedMs=milliseconds(start); return result;
    }();
    return cached;
}
struct Texture {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    Extent extent;
    std::uint64_t allocatedBytes=0;
};
void transition(ID3D12GraphicsCommandList* list,Texture& t,D3D12_RESOURCE_STATES to) {
    if (!t.resource || t.state == to) return;
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={t.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,t.state,to};
    list->ResourceBarrier(1,&b); t.state=to;
}
void uavBarrier(ID3D12GraphicsCommandList* list,Texture& t) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource=t.resource.Get();
    list->ResourceBarrier(1,&b);
}
ComPtr<IDXGIAdapter1> adapterFor(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi; hr(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"Query OBS DXGI device");
    ComPtr<IDXGIAdapter> adapter; hr(dxgi->GetAdapter(&adapter),"Read OBS rendering adapter");
    ComPtr<IDXGIAdapter1> result; hr(adapter.As(&result),"Query OBS adapter"); return result;
}
}
void warmShaderBytecode() { (void)shaderBytecode(); }
std::string Diagnostics::report() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << "DLSS5 Neural Filter\nstate=" << state << "\nerror=" << error << "\nfirstError=" << firstError
        << "\nsource=" << source.width << 'x' << source.height << "\ninference=" << inference.width << 'x' << inference.height
        << "\n" << identity << "\nflow=" << flow << "; valid=" << flowValid
        << "\nframes=" << frames << "; Evaluate=" << evaluations << "; cacheHits=" << cacheHits
        << "; resets=" << resets << "; passthrough=" << passthrough
        << "; temporalCommits=" << temporalCommits << "; nrMotionValid=" << nrMotionValid
        << "; exactInputComparisons=" << inputComparisons
        << "\nrenderRequests=" << renderRequests << "; completedFrames=" << completedFrames
        << "; displays=" << displays << "; busy=" << busy << "; skippedCandidates=" << droppedCandidates
        << "\nsubmittedFence=" << submittedFence << "; completedFence=" << completedFence
        << "; displayedFrame=" << displayedFrame << "; outputAgeMs=" << outputAgeMs
        << "; outputAvailable=" << outputAvailable
        << "\nrequestedModelPreset=" << modelPreset << "; actualModel=unknown; processingMode=" << processingMode
        << "; updateInterval=" << updateInterval << "; currentPercent=" << currentPercent
        << "; fullUpdates=" << fullUpdates << "; lightweightComposites=" << lightweightComposites
        << "; residualAge=" << residualAge << "; adaptive=" << adaptiveReason
        << "\ninitializationMs=" << initializationMs << "; shaderCompileMs=" << shaderCompileMs
        << "; featureCreateMs=" << featureCreateMs
        << "\ncpuRecordSubmitMs=" << cpuSubmitMs << "; allocatorWaitMs=" << waitMs
        << "\ngpuPrepareMs=" << gpuPrepareMs << "; gpuNrMs=" << gpuNrMs
        << "; gpuPostMs=" << gpuPostMs << "; gpuTemporalMs=" << gpuTemporalMs
        << "; gpuD3D12TotalMs=" << gpuTotalMs
        << "\nownedTextureAllocationBytes=" << allocatedBytes
        << "\nGPU timings cover D3D12 stages; OBS capture/draw and D3D11 copies are not included."
        << "\nNGX private allocations and optical-flow context allocations are additional.";
    return out.str();
}
struct GpuBackend::Impl {
    mutable std::mutex mutex;
    Diagnostics diag;
    BackendTestMode mode;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Texture2D> sharedInput11,sharedOutput11;
    ComPtr<ID3D11Fence> inputFence11,outputFence11;
    ComPtr<ID3D12Device> device;
    std::shared_ptr<NgxSession> ngx;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12CommandAllocator>,2> allocator;
    std::array<ComPtr<ID3D12GraphicsCommandList>,2> list;
    ComPtr<ID3D12Fence> inputFence,completed;
    std::uint64_t inputValue=0,submitted=0,obsDrainValue=0;
    HANDLE event=nullptr;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,ShaderCount> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    unsigned descriptorSize=0,descriptorCursor=0;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timingReadback;
    std::uint64_t frequency=0;
    bool timingsPending=false,initialized=false,failed=false,forceReset=true,nvResetPending=true;
    Texture input,output,proxy,zeroMotion,depth,motion,reducedDelta,metrics,stable,previousDelta,previousInput,previousProxy,flowColor,previousFlowColor;
    Texture changedFlag;
    ComPtr<ID3D12Resource> changedReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT changedFootprint{};
    ComPtr<ID3D11Texture2D> flowColor11,previousFlowColor11;
    struct Pass {
        Texture output;
        NVSDK_NGX_Parameter* parameters=nullptr;
        NVSDK_NGX_Handle* handle=nullptr;
        bool reset=true;
    };
    std::array<Pass,3> passes;
    std::unique_ptr<FlowGpu> flow;
    Settings previousSettings;
    Settings preparedSettings;
    int preparedPasses=0;
    FrameKey lastKey;
    std::uint64_t previousTime=0;
    bool hasInput=false,hasNeural=false,hasHistory=false,constantZerosInitialized=false;
    bool realtime=false,ready=false,resetPending=false;
    bool reuseOnly=false;
    std::unique_ptr<Impl> lightweight;
    detail::InputClock clock;
    detail::AdaptiveResolution adaptive;
    std::uint64_t lastFullSerial=0,submittedSerial=0,submittedEpoch=0;
    int lastProcessingMode=0;
    struct Snapshot {
        Texture proxy,result,reference;
        FrameKey key;
        Settings settings;
        std::uint64_t serial=0,epoch=0,time=0,fence=0,readFence=0;
        bool valid=false;
    };
    std::array<Snapshot,2> snapshots;
    int snapshotWrite=-1;
    std::array<Texture,3> motionChain;
    unsigned validLinks=0;
    std::uint64_t lastLightSerial=0,lastLightEpoch=0;
    bool lightInvalid=false,lightNeedsUpdate=false;
    Texture alternateOutput;
    ComPtr<ID3D11Texture2D> alternateOutput11;
    enum class Pending { None, Frame, Verify };
    Pending pending=Pending::None;
    int readySlot=0,writeSlot=0,displaySlot=0;
    std::array<std::uint64_t,2> slotReadFence{},slotFrame{},slotTime{};
    std::array<std::uint64_t,2> slotEpoch{};
    std::uint64_t requiredEpoch=0;
    std::array<Settings,2> slotSettings{};
    std::array<FrameKey,2> slotKey{};
    std::array<bool,2> slotInvalidateDisplay{};
    std::array<bool,2> slotHasResidual{};
    std::uint64_t outputSerial=0, displayCycle=0;
    std::uint64_t lastBusyCycle=0;
    bool displayFrozen=false,verificationReady=false,verificationChanged=false;
    bool degraded=false;
    unsigned recoveryFrames=0;
    ComPtr<ID3D12Fence> testDelayFence;
    std::uint64_t testDelayValue=0;
    unsigned testDelayMs=0;
    std::jthread testDelayWorker;
    // A single pending Verify owns the comparison/readback until its fence
    // retires; controls may change without changing those captured pixels.
    FrameKey verificationKey;
    Extent sourceExtent,inference;
    unsigned waitBudgetMs=3;
    explicit Impl(BackendTestMode value,unsigned timeout=3,BackendScheduling scheduling=BackendScheduling::Immediate)
        : mode(value),realtime(scheduling==BackendScheduling::CompletedFrames),waitBudgetMs(timeout) {}
    ~Impl() {
        if (ngx) for (auto& p : passes) ngx->release(p.parameters,p.handle);
        if (event) CloseHandle(event);
    }
    bool inFlight() const noexcept {
        if (lightweight && lightweight->inFlight()) return true;
        if (completed && completed->GetCompletedValue() < submitted) return true;
        if (inputFence && inputFence->GetCompletedValue() < obsDrainValue) return true;
        return flow && flow->fence() && flow->fence()->GetCompletedValue() < flow->fenceValue();
    }
    void wait(unsigned timeout,bool waitObs=false) {
        if (flow) flow->check();
        const bool gpuPending=(completed && completed->GetCompletedValue()<submitted) ||
            (flow && flow->fence() && flow->fence()->GetCompletedValue()<flow->fenceValue());
        if (!gpuPending && !(waitObs && inputFence && inputFence->GetCompletedValue()<obsDrainValue)) {
            if (flow) flow->check();
            if (waitObs && lightweight) lightweight->wait(timeout,true);
            return;
        }
        const auto start=Clock::now();
        if (completed->GetCompletedValue() < submitted) {
            if (!waitForFence(completed.Get(),submitted,event,timeout))
                throw Busy("GPU completion is pending");
        }
        if (flow && flow->fence() && flow->fence()->GetCompletedValue() < flow->fenceValue()) {
            const auto elapsed=milliseconds(start);
            const unsigned remaining=elapsed>=timeout ? 0 : timeout-static_cast<unsigned>(elapsed);
            if (!waitForFence(flow->fence(),flow->fenceValue(),event,remaining))
                throw Busy("Optical-flow completion is pending");
        }
        if (waitObs && inputFence && inputFence->GetCompletedValue() < obsDrainValue) {
            const auto elapsed=milliseconds(start);
            const unsigned remaining=elapsed>=timeout ? 0 : timeout-static_cast<unsigned>(elapsed);
            if (!waitForFence(inputFence.Get(),obsDrainValue,event,remaining))
                throw Busy("OBS output copy is pending");
        }
        diag.waitMs=milliseconds(start);
        if (flow) flow->check();
        if (waitObs && lightweight) {
            const auto elapsed=milliseconds(start);
            lightweight->wait(elapsed>=timeout ? 0 : timeout-static_cast<unsigned>(elapsed),true);
        }
    }
    Texture texture(Extent e,DXGI_FORMAT format,bool shared=false,ComPtr<ID3D11Texture2D>* native=nullptr) {
        Texture t; t.extent=e; t.format=format;
        if (shared) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=e.width; desc.Height=e.height;
            desc.ArraySize=1; desc.MipLevels=1; desc.Format=format; desc.SampleDesc.Count=1;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
            hr(device11->CreateTexture2D(&desc,nullptr,native->ReleaseAndGetAddressOf()),"Create OBS shared texture");
            ComPtr<IDXGIResource1> dxgi; hr(native->As(&dxgi),"Query shared texture");
            HANDLE handle=nullptr;
            hr(dxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"Share OBS texture");
            const auto result=device->OpenSharedHandle(handle,IID_PPV_ARGS(&t.resource));
            CloseHandle(handle); hr(result,"Open OBS texture in D3D12");
        } else {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width=e.width; desc.Height=e.height; desc.DepthOrArraySize=1; desc.MipLevels=1;
            desc.SampleDesc.Count=1; desc.Format=format; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                IID_PPV_ARGS(&t.resource)),"Allocate neural texture");
        }
        const auto desc=t.resource->GetDesc();
        t.allocatedBytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        diag.allocatedBytes+=t.allocatedBytes;
        return t;
    }
    void createSharedFence(ComPtr<ID3D12Fence>& gpu,ComPtr<ID3D11Fence>& obs) {
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&gpu)),"Create shared graphics fence");
        HANDLE handle=nullptr;
        hr(device->CreateSharedHandle(gpu.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"Share graphics fence");
        const auto result=device11->OpenSharedFence(handle,IID_PPV_ARGS(&obs));
        CloseHandle(handle); hr(result,"Open graphics fence in OBS");
    }
    void compile() {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,7,0,0,0};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,0};
        D3D12_ROOT_PARAMETER params[3]{};
        for (int i=0;i<2;++i) {
            params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable={1,&ranges[i]}; params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        }
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants={0,0,static_cast<unsigned>(sizeof(Parameters)/4)};
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=3; desc.pParameters=params;
        desc.NumStaticSamplers=1; desc.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> blob,error;
        hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Serialize shader root signature");
        hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create shader root signature");
        const auto& cached=shaderBytecode(); diag.shaderCompileMs=cached.elapsedMs;
        for (unsigned i=0;i<ShaderCount;++i) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root.Get();
            pso.CS={cached.bytes[i].data(),cached.bytes[i].size()};
            hr(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline[i])),"Create compute pipeline");
        }
    }
    void initialize(ID3D11Device* obs,Extent source,const Settings& settings,ID3D12Device* sharedDevice=nullptr,
                    ID3D11DeviceContext4* sharedContext=nullptr) {
        const auto initializeStart=Clock::now();
        previousSettings=preparedSettings=settings;
        sourceExtent=source; inference=inferenceExtent(source,settings);
        if (!safeExtent(inference) || source.width > 3840 || source.height > 2160)
            throw std::runtime_error("Inference safety envelope: 256x144 through 3840x2160; lower source scaling or size is unsupported");
        hr(obs->QueryInterface(IID_PPV_ARGS(&device11)),"OBS device lacks ID3D11Device5 shared-fence capability");
        if (sharedContext) context11=sharedContext;
        else {
            ComPtr<ID3D11DeviceContext> context; obs->GetImmediateContext(&context);
            hr(context.As(&context11),"OBS context lacks ID3D11DeviceContext4 shared-fence capability");
        }
        auto adapter=adapterFor(obs);
        if (sharedDevice) { device=sharedDevice; diag.identity=ngx ? ngx->identity : "private residual-composition queue"; }
        else if (mode == BackendTestMode::Neural) { ngx=NgxSession::acquire(adapter.Get(),settings.runtime); device=ngx->device; diag.identity=ngx->identity; }
        else { hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Create identity-test D3D12 device"); diag.identity="TEST BACKEND: not neural inference"; }
        D3D12_COMMAND_QUEUE_DESC q{};
        ComPtr<IDXGIDevice> hostDxgi;
        INT hostPriority=0;
        if (SUCCEEDED(obs->QueryInterface(IID_PPV_ARGS(&hostDxgi)))) hostDxgi->GetGPUThreadPriority(&hostPriority);
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Create private graphics queue");
        diag.identity+="; hostGpuPriority="+std::to_string(hostPriority)+"; privateQueuePriority="+std::to_string(q.Priority);
        for (int i=0;i<2;++i) {
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator[i])),"Create graphics allocator");
            hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator[i].Get(),nullptr,IID_PPV_ARGS(&list[i])),"Create graphics list");
            hr(list[i]->Close(),"Close initial list");
        }
        createSharedFence(inputFence,inputFence11); createSharedFence(completed,outputFence11);
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!event) throw std::runtime_error("Create graphics completion event failed");
        input=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT,true,&sharedInput11);
        // Shared output only receives a copy; UAV writes use a private FP16 texture.
        output=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT,true,&sharedOutput11);
        if (realtime) alternateOutput=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT,true,&alternateOutput11);
        proxy=texture(inference,DXGI_FORMAT_R8G8B8A8_UNORM);
        previousProxy=texture(inference,DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!reuseOnly) for (auto& p : passes) p.output=texture(inference,DXGI_FORMAT_R8G8B8A8_UNORM);
        zeroMotion=texture(inference,DXGI_FORMAT_R32G32_FLOAT);
        depth=texture(inference,DXGI_FORMAT_R32_FLOAT);
        motion=texture(inference,DXGI_FORMAT_R32G32_FLOAT);
        reducedDelta=texture(inference,DXGI_FORMAT_R16G16B16A16_FLOAT);
        metrics=texture(inference,DXGI_FORMAT_R16G16B16A16_FLOAT);
        stable=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT);
        previousDelta=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT);
        previousInput=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT);
        composite=texture(source,DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (reuseOnly) for (auto& link : motionChain) link=texture(inference,DXGI_FORMAT_R32G32_FLOAT);
        changedFlag=texture({1,1},DXGI_FORMAT_R32_UINT);
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=1024; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors)),"Create shader descriptor arena");
        descriptorSize=device->GetDescriptorHandleIncrementSize(heap.Type);
        compile();
        D3D12_QUERY_HEAP_DESC query{}; query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count=12;
        hr(device->CreateQueryHeap(&query,IID_PPV_ARGS(&queries)),"Create GPU timestamp queries");
        D3D12_HEAP_PROPERTIES readback{}; readback.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=12*sizeof(std::uint64_t); buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
        buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr(device->CreateCommittedResource(&readback,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&timingReadback)),"Allocate timing readback");
        hr(queue->GetTimestampFrequency(&frequency),"Query GPU timestamp frequency");
        const auto flagDesc=changedFlag.resource->GetDesc();
        UINT64 flagBytes=0;
        device->GetCopyableFootprints(&flagDesc,0,1,0,&changedFootprint,nullptr,nullptr,&flagBytes);
        buffer.Width=flagBytes;
        hr(device->CreateCommittedResource(&readback,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&changedReadback)),"Allocate input-verification metadata");
        flow=std::make_unique<FlowGpu>(); configureFlow(settings);
        diag.source=source; diag.inference=inference;
        initialized=true; previousSettings=settings; diag.initializationMs=milliseconds(initializeStart);
    }
    Texture composite,reprojected;
    void configureFlow(const Settings& settings) {
        diag.allocatedBytes-=flowColor.allocatedBytes+previousFlowColor.allocatedBytes;
        flowColor={}; previousFlowColor={}; flowColor11.Reset(); previousFlowColor11.Reset();
        try {
            flow->initialize(device.Get(),device11.Get(),inference,settings);
            if (flow->method()) {
                if (flow->method()==2) {
                    flowColor=texture(flow->colorExtent(),flow->colorFormat(),true,&flowColor11);
                    previousFlowColor=texture(flow->colorExtent(),flow->colorFormat(),true,&previousFlowColor11);
                    flow->prepareNvInputs(flowColor.resource.Get(),previousFlowColor.resource.Get(),flowColor11.Get(),previousFlowColor11.Get());
                } else flowColor=texture(flow->colorExtent(),flow->colorFormat());
            }
            diag.flow=flow->description();
        } catch (const std::exception& e) {
            Settings none=settings; none.flow=0; none.af=0;
            flow->initialize(device.Get(),device11.Get(),inference,none);
            diag.flow=std::string("unavailable; fallback=zero-NR-motion/static-AF; ")+e.what();
            writeDiagnosticLog(diag.flow);
        }
        preparedSettings.flow=settings.flow; preparedSettings.amdQuality=settings.amdQuality;
        preparedSettings.nvQuality=settings.nvQuality; preparedSettings.af=settings.af;
    }
    ID3D12GraphicsCommandList* begin(int index) {
        hr(allocator[index]->Reset(),"Reset completed allocator");
        hr(list[index]->Reset(allocator[index].Get(),nullptr),"Reset completed command list");
        return list[index].Get();
    }
    void submit(int index) {
        hr(list[index]->Close(),"Close graphics list");
        ID3D12CommandList* command[]{list[index].Get()}; queue->ExecuteCommandLists(1,command);
        hr(queue->Signal(completed.Get(),++submitted),"Signal graphics completion");
    }
    void timestamp(ID3D12GraphicsCommandList* command,unsigned index) { command->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index); }
    void harvest() {
        if (!timingsPending) return;
        void* data=nullptr; D3D12_RANGE range{0,12*sizeof(std::uint64_t)};
        hr(timingReadback->Map(0,&range,&data),"Read completed GPU timestamps");
        const auto* ticks=static_cast<const std::uint64_t*>(data);
        auto duration=[&](int a,int b) { return ticks[b] >= ticks[a] ? (ticks[b]-ticks[a])*1000.0/frequency : 0.; };
        diag.gpuPrepareMs=duration(0,1); diag.gpuNrMs=duration(4,5);
        diag.gpuPostMs=duration(6,7); diag.gpuTemporalMs=duration(8,9); diag.gpuTotalMs=duration(0,11);
        D3D12_RANGE none{0,0}; timingReadback->Unmap(0,&none); timingsPending=false;
        if (!realtime && !reuseOnly) ++diag.completedFrames;
    }
    void dispatch(ID3D12GraphicsCommandList* command,Shader shader,Parameters params,Extent extent,
                  const std::array<Texture*,7>& srvs={},const std::array<Texture*,4>& uavs={}) {
        if (descriptorCursor+11 > 1024) throw std::runtime_error("Shader descriptor arena exhausted");
        const unsigned base=descriptorCursor; descriptorCursor+=11;
        auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr+=static_cast<SIZE_T>(base)*descriptorSize;
        for (unsigned i=0;i<7;++i) {
            auto* texture=srvs[i];
            if (texture) transition(command,*texture,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Format=texture ? texture->format : (i == 6 ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16B16A16_FLOAT);
            d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(texture ? texture->resource.Get() : nullptr,&d,cpu);
            cpu.ptr+=descriptorSize;
        }
        for (unsigned i=0;i<4;++i) {
            auto* texture=uavs[i];
            if (texture) transition(command,*texture,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Format=texture ? texture->format : (i == 2 ? DXGI_FORMAT_R32G32_FLOAT :
                                                  i == 3 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT);
            device->CreateUnorderedAccessView(texture ? texture->resource.Get() : nullptr,nullptr,&d,cpu);
            cpu.ptr+=descriptorSize;
        }
        auto gpu=descriptors->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=static_cast<UINT64>(base)*descriptorSize;
        ID3D12DescriptorHeap* heaps[]{descriptors.Get()}; command->SetDescriptorHeaps(1,heaps);
        command->SetComputeRootSignature(root.Get()); command->SetPipelineState(pipeline[shader].Get());
        command->SetComputeRootDescriptorTable(0,gpu); gpu.ptr+=7ull*descriptorSize;
        command->SetComputeRootDescriptorTable(1,gpu);
        command->SetComputeRoot32BitConstants(2,sizeof(params)/4,&params,0);
        command->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);
        for (auto* texture : uavs) if (texture) uavBarrier(command,*texture);
    }
    void copy(ID3D12GraphicsCommandList* command,Texture& destination,Texture& source) {
        transition(command,source,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(command,destination,D3D12_RESOURCE_STATE_COPY_DEST);
        command->CopyResource(destination.resource.Get(),source.resource.Get());
    }
    Parameters parameters(const Settings& settings,std::uint64_t now,bool history,bool motionValid) {
        Parameters p; p.source=sourceExtent; p.inference=inference;
        p.mix=settings.mix; p.strength=settings.strength; p.chroma=settings.chroma; p.lightness=settings.lightness;
        p.darken=settings.darken; p.brighten=settings.brighten; p.hue=settings.hue;
        p.darkProtect=settings.darkProtect; p.brightProtect=settings.brightProtect; p.compression=settings.compression;
        p.low=settings.low; p.high=settings.high; p.view=settings.view; p.debug=settings.debug; p.af=settings.af;
        p.midtone=settings.midtone; p.chromaProtection=settings.chromaProtection;
        // Missing flow is an explicit static fallback, never stale vector reuse.
        if (p.af >= 2 && !flow->method()) p.af=1;
        p.weight=historyWeight(previousTime,now,settings.responseMs,settings.afStrength);
        p.appearWeight=historyWeight(previousTime,now,180,settings.afStrength);
        p.history=history; p.motion=motionValid; p.testDelta=mode == BackendTestMode::ControlledDelta ? .03f : 0;
        p.recoveryFlags=(settings.processingMode==1 && settings.flow==2 && flow->method()==2) ? 1u : 0u;
        if (flow->method()) {
            p.flow=flow->vectorExtent(); p.flowColor=flow->colorExtent();
            p.flowGrid=flow->grid(); p.flowDivisor=flow->divisor();
        }
        return p;
    }
    Texture& frameOutput(int slot) { return slot ? alternateOutput : output; }
    ID3D11Texture2D* frameOutput11(int slot) { return slot ? alternateOutput11.Get() : sharedOutput11.Get(); }
    void poll(std::uint64_t now) {
        if (!initialized) return;
        if (flow) flow->check();
        diag.submittedFence=submitted; diag.completedFence=completed->GetCompletedValue();
        if (diag.completedFence==UINT64_MAX) throw std::runtime_error("GPU fence reports device removal");
        if (pending==Pending::None || diag.completedFence<submitted) return;
        if (flow) flow->check();
        if (pending==Pending::Frame) {
            harvest(); ready=true; readySlot=writeSlot; ++diag.completedFrames;
            if (snapshotWrite>=0) { snapshots[snapshotWrite].valid=true; snapshotWrite=-1; }
            if (slotInvalidateDisplay[readySlot]) displayFrozen=false;
            if (now>=slotTime[readySlot] && now-slotTime[readySlot]<100000000ull) ++recoveryFrames;
            else recoveryFrames=0;
        } else {
            void* data=nullptr; D3D12_RANGE range{0,sizeof(unsigned)};
            hr(changedReadback->Map(0,&range,&data),"Read completed input verification");
            verificationChanged=*static_cast<const unsigned*>(data)!=0;
            D3D12_RANGE none{0,0}; changedReadback->Unmap(0,&none);
            verificationReady=true; ++diag.inputComparisons;
        }
        pending=Pending::None;
    }
    bool display(ID3D11Texture2D* destination,FrameKey key,std::uint64_t now,bool busyState=false) {
        diag.outputAvailable=false;
        if (!ready || key.extent!=sourceExtent || (reuseOnly && slotEpoch[readySlot]!=requiredEpoch) ||
            slotKey[readySlot].generation!=key.generation || slotKey[readySlot].inputRevision!=key.inputRevision) {
            diag.state=busyState ? "busy" : "initializing"; return false;
        }
        if (!displayFrozen || displayCycle!=key.cycle || degraded ||
            slotKey[displaySlot].generation!=key.generation || slotKey[displaySlot].inputRevision!=key.inputRevision ||
            (reuseOnly && slotEpoch[displaySlot]!=requiredEpoch)) {
            displaySlot=readySlot; displayCycle=key.cycle; displayFrozen=true;
        }
        diag.outputAgeMs=now>slotTime[displaySlot] ? (now-slotTime[displaySlot])/1000000. : 0;
        if (diag.outputAgeMs>500) { if (!degraded) recoveryFrames=0; degraded=true; }
        if (degraded && recoveryFrames>=3) degraded=false;
        if (degraded) { diag.state="degraded"; return false; }
        // This slot's D3D12 fence is already complete. Never add a wait for
        // current inference to the OBS immediate context.
        context11->CopyResource(destination,frameOutput11(displaySlot));
        slotReadFence[displaySlot]=obsDrainValue=++inputValue;
        hr(context11->Signal(inputFence11.Get(),obsDrainValue),"Signal displayed OBS copy");
        context11->Flush();
        ++diag.displays; diag.displayedFrame=slotFrame[displaySlot]; diag.outputAvailable=true;
        diag.state=busyState ? "busy" : "processing"; diag.error.clear(); return true;
    }
    bool hold(ID3D11Texture2D* destination,FrameKey key,std::uint64_t now) {
        ++diag.busy;
        if (key.cycle!=lastKey.cycle && key.cycle!=lastBusyCycle) { ++diag.droppedCandidates; lastBusyCycle=key.cycle; }
        return display(destination,key,now,true);
    }
    void ensureReuse(const Settings& settings) {
        if (!snapshots[0].proxy.resource) for (auto& snapshot : snapshots) {
            snapshot.proxy=texture(inference,DXGI_FORMAT_R8G8B8A8_UNORM);
            snapshot.result=texture(inference,DXGI_FORMAT_R8G8B8A8_UNORM);
            snapshot.reference=texture(sourceExtent,DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
        if (!lightweight) {
            lightweight=std::make_unique<Impl>(mode,waitBudgetMs,realtime ? BackendScheduling::CompletedFrames : BackendScheduling::Immediate);
            lightweight->reuseOnly=true;
            lightweight->initialize(device11.Get(),sourceExtent,settings,device.Get(),context11.Get());
            lightweight->reprojected=lightweight->texture(inference,DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
    }
    void prepare(ID3D11Device* obs,Extent source,const Settings& settings,
                 std::shared_ptr<NgxSession> session,ID3D12Device* sharedDevice,ID3D11DeviceContext4* context,
                 const std::function<bool()>& cancelled) {
        ngx=std::move(session);
        initialize(obs,source,settings,sharedDevice,context);
        if (cancelled()) return;
        if (settings.processingMode) ensureReuse(settings);
        if (cancelled()) return;
        if (!ngx) { preparedPasses=settings.passes; return; }
        auto* command=begin(0);
        for (int i=0;i<settings.passes;++i) {
            const auto start=Clock::now();
            {
                // Other live features may record between individual creates.
                // No preparation/retirement mutex is held across this API call.
                std::scoped_lock lock(ngx->execution);
                if (cancelled()) break;
                auto& pass=passes[i];
                ngx->create(command,inference,&pass.parameters,&pass.handle,settings.modelPreset);
                pass.reset=true;
            }
            diag.featureCreateMs+=milliseconds(start);
            preparedPasses=i+1;
        }
        if (!preparedPasses) return;
        submit(0);
        // CreateFeature may record GPU initialization. The worker owns this
        // allocator until that work completes, before publishing the candidate.
        wait(3000);
    }
    void publishSnapshot(ID3D12GraphicsCommandList* command,const Settings& settings,FrameKey key,std::uint64_t now) {
        int selected=-1;
        for (int i=0;i<static_cast<int>(snapshots.size());++i) {
            auto& snapshot=snapshots[i];
            if (!snapshot.proxy.resource || (lightweight && snapshot.readFence>lightweight->completed->GetCompletedValue())) continue;
            if (selected<0 || snapshot.serial<snapshots[selected].serial) selected=i;
        }
        // There is at most one light submission in flight. Two immutable
        // pairs therefore always leave one writer, without queueing a wait.
        if (selected<0) throw std::runtime_error("No retired residual snapshot is available");
        auto& snapshot=snapshots[selected]; snapshot.valid=false;
        copy(command,snapshot.proxy,proxy); copy(command,snapshot.result,passes[settings.passes-1].output);
        copy(command,snapshot.reference,input);
        transition(command,snapshot.proxy,D3D12_RESOURCE_STATE_COMMON);
        transition(command,snapshot.result,D3D12_RESOURCE_STATE_COMMON);
        transition(command,snapshot.reference,D3D12_RESOURCE_STATE_COMMON);
        transition(command,input,D3D12_RESOURCE_STATE_COMMON);
        snapshot.key=key; snapshot.settings=settings; snapshot.time=now;
        snapshot.serial=submittedSerial; snapshot.epoch=submittedEpoch; snapshotWrite=selected;
    }
    Snapshot* usableSnapshot(const Settings& settings,FrameKey key) {
        Snapshot* selected=nullptr;
        for (auto& snapshot : snapshots) {
            if (!snapshot.valid || snapshot.fence>completed->GetCompletedValue() || snapshot.epoch!=clock.epoch ||
                snapshot.key.generation!=key.generation || snapshot.key.inputRevision!=key.inputRevision ||
                snapshot.settings.passes!=settings.passes || snapshot.settings.modelPreset!=settings.modelPreset ||
                settings.firstDirty(snapshot.settings)<settings.passes || clock.serial<snapshot.serial) continue;
            if (!selected || snapshot.serial>selected->serial) selected=&snapshot;
        }
        diag.residualAge=selected ? clock.serial-selected->serial : 0;
        return selected && diag.residualAge<=3 ? selected : nullptr;
    }
    bool composeHistory(ID3D11Texture2D* original,ID3D11Texture2D* destination,const Settings& settings,
                        FrameKey key,std::uint64_t now,std::uint64_t serial,std::uint64_t epoch,
                        Snapshot* snapshot,bool reset,bool allowNewInput) {
        if (requiredEpoch!=epoch) displayFrozen=false;
        requiredEpoch=epoch;
        if (realtime) poll(now);
        if (pending!=Pending::None) return hold(destination,key,now);
        if (!allowNewInput && (!hasInput || key!=lastKey)) return display(destination,key,now);
        diag.waitMs=0; wait(realtime ? 0 : waitBudgetMs); harvest();
        bool same=hasInput && key==lastKey;
        const bool outletChanged=settings.outletKey()!=previousSettings.outletKey();
        bool verified=false;
        if (verificationReady) {
            verified=verificationKey==key;
            if (verified && verificationChanged) {
                same=false; reset=true; lightInvalid=true; snapshot=nullptr; displayFrozen=false;
            }
            verificationReady=false;
        }
        descriptorCursor=0;
        if (same && !reset && !forceReset && !verified) {
            // Verify a second view's capture without advancing the cycle or AF.
            context11->CopyResource(sharedInput11.Get(),original);
            hr(context11->Signal(inputFence11.Get(),++inputValue),"Signal light input verification"); context11->Flush();
            hr(queue->Wait(inputFence.Get(),inputValue),"Wait light input verification"); input.state=D3D12_RESOURCE_STATE_COMMON;
            auto* command=begin(0); Parameters params; params.source=sourceExtent; params.inference=inference;
            dispatch(command,ClearChanged,params,{1,1},{},{&changedFlag});
            dispatch(command,CompareInput,params,sourceExtent,{&input,&previousInput},{&changedFlag});
            transition(command,changedFlag,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=changedReadback.Get();
            dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=changedFootprint;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=changedFlag.resource.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            command->CopyTextureRegion(&dst,0,0,0,&src,nullptr); transition(command,input,D3D12_RESOURCE_STATE_COMMON); submit(0);
            if (realtime) {
                pending=Pending::Verify; verificationKey=key;
                return display(destination,key,now);
            }
            wait(waitBudgetMs);
            void* data=nullptr; D3D12_RANGE range{0,sizeof(unsigned)};
            hr(changedReadback->Map(0,&range,&data),"Read light input-verification flag");
            const bool changed=*static_cast<const unsigned*>(data)!=0;
            D3D12_RANGE none{0,0}; changedReadback->Unmap(0,&none); ++diag.inputComparisons;
            if (changed) { same=false; reset=true; lightInvalid=true; snapshot=nullptr; }
            descriptorCursor=0;
        }
        if (same && !reset && !forceReset && !outletChanged) {
            ++diag.cacheHits;
            if (realtime) return display(destination,key,now);
            hr(context11->Wait(outputFence11.Get(),submitted),"Wait cached light output");
            context11->CopyResource(destination,sharedOutput11.Get());
            obsDrainValue=++inputValue; hr(context11->Signal(inputFence11.Get(),obsDrainValue),"Signal cached light copy");
            context11->Flush(); return true;
        }
        writeSlot=realtime && ready ? 1-readySlot : 0;
        if (realtime) {
            if (displayFrozen && displayCycle==key.cycle && writeSlot==displaySlot) {
                // A newer completion may arrive after this preview froze.
                // Preserve its displayed slot while refreshing the other slot.
                readySlot=displaySlot; writeSlot=1-displaySlot;
            }
            if (inputFence->GetCompletedValue()<slotReadFence[writeSlot]) return hold(destination,key,now);
        }
        const auto start=Clock::now();
        reset=reset || forceReset || lastLightEpoch!=epoch || (!same && !temporalPair(previousTime,now,false));
        if (reset) nvResetPending=true;
        if (reset || outletChanged) hasHistory=false;
        const bool contiguous=hasInput && lastLightSerial+1==serial && lastLightEpoch==epoch;
        const bool cachedMotion=same && lastLightSerial==serial && lastLightEpoch==epoch && diag.flowValid;
        const bool motionValid=!reset && flow->method() && ((contiguous && !same && flow->readyForMotion()) || cachedMotion);
        auto params=parameters(settings,now,hasHistory,motionValid);
        context11->CopyResource(sharedInput11.Get(),original);
        hr(context11->Signal(inputFence11.Get(),++inputValue),"Signal current light input"); context11->Flush();
        hr(queue->Wait(inputFence.Get(),inputValue),"Wait current light capture"); input.state=D3D12_RESOURCE_STATE_COMMON;
        auto* command=begin(0); timestamp(command,0);
        dispatch(command,Proxy,params,inference,{&input},{&proxy});
        if (!motionValid && params.af!=0) dispatch(command,Zero,params,inference,{}, {nullptr,nullptr,&motion,nullptr});
        timestamp(command,1); timestamp(command,2);
        if ((!same || (reset && flow->readyForMotion())) && flow->method()) {
            if (flow->method()==1) {
                dispatch(command,FlowColor,params,flow->colorExtent(),{&proxy},{&flowColor});
                transition(command,flowColor,D3D12_RESOURCE_STATE_COMMON);
                flow->recordAmd(command,flowColor.resource.Get(),reset || !contiguous);
            } else if (motionValid) {
                dispatch(command,FlowLuma,params,flow->colorExtent(),{&proxy},{nullptr,nullptr,nullptr,&flowColor});
                dispatch(command,FlowLuma,params,flow->colorExtent(),{&previousProxy},{nullptr,nullptr,nullptr,&previousFlowColor});
                transition(command,flowColor,D3D12_RESOURCE_STATE_COMMON); transition(command,previousFlowColor,D3D12_RESOURCE_STATE_COMMON);
                submit(0);
                flow->submitNv(flowColor.resource.Get(),previousFlowColor.resource.Get(),flowColor11.Get(),previousFlowColor11.Get(),
                    completed.Get(),submitted,nvResetPending);
                nvResetPending=false;
                hr(queue->Wait(flow->fence(),flow->fenceValue()),"Wait light NVOF completion"); command=begin(1);
            }
            if (motionValid) {
                Texture vectors; vectors.resource=flow->vectors(); vectors.extent=flow->vectorExtent(); vectors.format=DXGI_FORMAT_R16G16_SINT;
                dispatch(command,Dense,params,inference,{nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&vectors},{nullptr,nullptr,&motion});
                transition(command,vectors,D3D12_RESOURCE_STATE_COMMON);
            }
        }
        if (!same) {
            if (motionValid) {
                auto spare=std::move(motionChain[2]); motionChain[2]=std::move(motionChain[1]);
                motionChain[1]=std::move(motionChain[0]); motionChain[0]=std::move(spare);
                copy(command,motionChain[0],motion); validLinks=std::min(3u,validLinks+1);
            } else validLinks=0;
        }
        timestamp(command,3); timestamp(command,4); timestamp(command,5); timestamp(command,6);
        Texture* currentDelta=&reducedDelta;
        if (snapshot) {
            dispatch(command,Delta,params,inference,{&snapshot->proxy,&snapshot->result},{&reducedDelta,&metrics});
            const auto age=serial-snapshot->serial;
            if (age) {
                auto reuseParams=params; reuseParams.residualAge=static_cast<unsigned>(age);
                reuseParams.residualReuse=settings.residualTransfer && settings.flow && flow->method() ? 2u : 1u;
                reuseParams.recoveryFlags=(reuseParams.residualReuse==2 && settings.flow==2 && flow->method()==2) ? 1u : 0u;
                reuseParams.motion=motionValid && validLinks>=age;
                dispatch(command,Reuse,reuseParams,inference,{&input,&reducedDelta,&snapshot->reference,
                    &motionChain[0],&motionChain[1],&motionChain[2]},{&reprojected});
                currentDelta=&reprojected;
                if (reuseParams.residualReuse==2 && !reuseParams.motion) lightNeedsUpdate=true;
            }
            // No parent writer may touch this pair until this light submission
            // completes. Return all cross-queue resources to COMMON first.
            transition(command,snapshot->proxy,D3D12_RESOURCE_STATE_COMMON);
            transition(command,snapshot->result,D3D12_RESOURCE_STATE_COMMON);
            transition(command,snapshot->reference,D3D12_RESOURCE_STATE_COMMON);
            diag.residualAge=age;
        } else {
            dispatch(command,Delta,params,inference,{&proxy,&proxy},{&reducedDelta,&metrics}); diag.residualAge=0;
        }
        timestamp(command,7); timestamp(command,8);
        Texture* displayDelta=currentDelta;
        if (params.af!=0 || sourceExtent!=inference) {
            dispatch(command,Temporal,params,sourceExtent,{currentDelta,&previousDelta,&input,&previousInput,&motion},{&stable});
            displayDelta=&stable;
        }
        timestamp(command,9); timestamp(command,10);
        dispatch(command,Compose,params,sourceExtent,{&input,displayDelta,&metrics},{&composite});
        auto& targetOutput=frameOutput(writeSlot); copy(command,targetOutput,composite);
        transition(command,targetOutput,D3D12_RESOURCE_STATE_COMMON);
        if (!same || !hasHistory) {
            if (params.af!=0) copy(command,previousDelta,stable);
            copy(command,previousInput,input);
            if (flow->method()) copy(command,previousProxy,proxy);
            hasHistory=true; if (!same) ++diag.temporalCommits;
        }
        transition(command,input,D3D12_RESOURCE_STATE_COMMON); timestamp(command,11);
        command->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,12,timingReadback.Get(),0);
        submit(command==list[1].Get() ? 1 : 0); timingsPending=true;
        if (snapshot) snapshot->readFence=submitted;
        ++diag.lightweightComposites;
        slotTime[writeSlot]=now; slotFrame[writeSlot]=++outputSerial; slotKey[writeSlot]=key; slotSettings[writeSlot]=settings;
        slotEpoch[writeSlot]=epoch;
        slotHasResidual[writeSlot]=snapshot!=nullptr;
        slotInvalidateDisplay[writeSlot]=same && outletChanged;
        if (realtime) pending=Pending::Frame;
        else {
            hr(context11->Wait(outputFence11.Get(),submitted),"Wait offline light result");
            context11->CopyResource(destination,sharedOutput11.Get());
            obsDrainValue=++inputValue; hr(context11->Signal(inputFence11.Get(),obsDrainValue),"Signal offline light copy"); context11->Flush();
        }
        if (!same) { ++diag.frames; previousTime=now; }
        diag.flowValid=motionValid; diag.cpuSubmitMs=milliseconds(start);
        lastKey=key; previousSettings=settings; hasInput=true; forceReset=false;
        lastLightSerial=serial; lastLightEpoch=epoch;
        return realtime ? display(destination,key,now) : true;
    }
    bool process(ID3D11Device* obs,ID3D11Texture2D* original,ID3D11Texture2D* destination,
                 const Settings& settings,FrameKey key,std::uint64_t now,bool reset) {
        if (failed) { ++diag.passthrough; return false; }
        ++diag.renderRequests;
        const auto priorEpoch=clock.epoch;
        const bool newCycle=clock.observe(key,now,reset);
        if (!initialized) initialize(obs,key.extent,settings);
        diag.modelPreset=settings.modelPreset; diag.processingMode=settings.processingMode; diag.updateInterval=settings.updateInterval;
        diag.currentPercent=settings.scaling ? settings.percent : 100;
        if (!settings.processingMode) {
            if (lastProcessingMode!=0) { forceReset=true; hasHistory=false; }
            lastProcessingMode=0;
            return processFull(obs,original,destination,settings,key,now,reset);
        }
        ensureReuse(settings);
        if (lastProcessingMode!=1 || priorEpoch!=clock.epoch) {
            forceReset=true; hasHistory=false; lightweight->forceReset=true; lightweight->hasHistory=false;
        }
        lastProcessingMode=1;
        if (lightweight->lightInvalid) {
            ++clock.epoch; lightweight->lightInvalid=false; forceReset=true; hasHistory=false;
            lightweight->forceReset=true; lightweight->hasHistory=false;
        }
        if (realtime) poll(now);
        else if (submitted && completed->GetCompletedValue()>=submitted) harvest();
        auto* snapshot=usableSnapshot(settings,key);
        const bool passChanged=hasInput && (settings.passes!=previousSettings.passes ||
            settings.firstDirty(previousSettings)<std::min(settings.passes,previousSettings.passes));
        const bool due=forceReset || reset || passChanged || !hasInput ||
            (newCycle && (!snapshot || lightweight->lightNeedsUpdate ||
                clock.serial-lastFullSerial>=static_cast<unsigned>(settings.updateInterval)));
        if (due && pending==Pending::None) {
            submittedSerial=clock.serial; submittedEpoch=clock.epoch;
            const auto before=diag.fullUpdates;
            try { processFull(obs,original,destination,settings,key,now,reset || priorEpoch!=clock.epoch); }
            catch (const Busy&) { ++diag.busy; resetPending=resetPending || reset; }
            if (diag.fullUpdates!=before) { lastFullSerial=clock.serial; lightweight->lightNeedsUpdate=false; }
            if (!realtime) wait(waitBudgetMs);
            snapshot=usableSnapshot(settings,key);
        } else if (pending!=Pending::None) {
            ++diag.busy;
            if (newCycle) { ++diag.droppedCandidates; lastBusyCycle=key.cycle; }
        }
        const auto beforeLight=lightweight->diag;
        const bool allowLight=newCycle || reset || priorEpoch!=clock.epoch ||
            settings.outletKey()!=lightweight->previousSettings.outletKey();
        const bool displayed=lightweight->composeHistory(original,destination,settings,key,now,clock.serial,clock.epoch,
            snapshot,reset || priorEpoch!=clock.epoch,allowLight);
        diag.lightweightComposites+=lightweight->diag.lightweightComposites-beforeLight.lightweightComposites;
        diag.temporalCommits+=lightweight->diag.temporalCommits-beforeLight.temporalCommits;
        diag.inputComparisons+=lightweight->diag.inputComparisons-beforeLight.inputComparisons;
        diag.cacheHits+=lightweight->diag.cacheHits-beforeLight.cacheHits;
        diag.displays+=lightweight->diag.displays-beforeLight.displays;
        diag.residualAge=lightweight->diag.residualAge;
        diag.outputAvailable=displayed; diag.outputAgeMs=lightweight->diag.outputAgeMs;
        diag.displayedFrame=lightweight->diag.displayedFrame;
        diag.state=displayed ? (pending==Pending::None ? "processing" : "busy") : "initializing";
        return displayed;
    }
    bool processFull(ID3D11Device* obs,ID3D11Texture2D* original,ID3D11Texture2D* destination,
                 const Settings& settings,FrameKey key,std::uint64_t now,bool reset) {
        if (failed) { ++diag.passthrough; return false; }
        if (!initialized) initialize(obs,key.extent,settings);
        if (realtime) {
            resetPending=resetPending || reset; poll(now);
            if (pending!=Pending::None) return hold(destination,key,now);
            writeSlot=ready ? 1-readySlot : 0;
            reset=resetPending;
        }
        diag.waitMs=0; wait(realtime ? 0 : waitBudgetMs); harvest();
        const auto start=Clock::now(); descriptorCursor=0;
        bool same=hasInput && key == lastKey;
        bool verified=false;
        if (realtime && verificationReady) {
            verified=verificationKey==key;
            if (verified && verificationChanged) { same=false; reset=true; displayFrozen=false; }
            verificationReady=false;
        }
        if (same && !reset && !forceReset && !verified) {
            // The OBS cycle is only a candidate identity. A target may render
            // different pixels in that cycle without emitting an update signal.
            // Compare the actual captured FP16 input before reusing neural data.
            context11->CopyResource(sharedInput11.Get(),original);
            hr(context11->Signal(inputFence11.Get(),++inputValue),"Signal duplicate-input verification");
            context11->Flush();
            hr(queue->Wait(inputFence.Get(),inputValue),"Wait duplicate-input capture");
            input.state=D3D12_RESOURCE_STATE_COMMON;
            auto* verify=begin(0);
            Parameters p; p.source=sourceExtent; p.inference=inference;
            dispatch(verify,ClearChanged,p,{1,1},{},{&changedFlag});
            dispatch(verify,CompareInput,p,sourceExtent,{&input,&previousInput},{&changedFlag});
            transition(verify,changedFlag,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=changedReadback.Get();
            dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=changedFootprint;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=changedFlag.resource.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            verify->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            transition(verify,input,D3D12_RESOURCE_STATE_COMMON);
            submit(0);
            if (realtime) {
                pending=Pending::Verify; verificationKey=key;
                return display(destination,key,now);
            }
            wait(waitBudgetMs);
            void* data=nullptr; D3D12_RANGE range{0,sizeof(unsigned)};
            hr(changedReadback->Map(0,&range,&data),"Read input-verification flag");
            const bool changed=*static_cast<const unsigned*>(data)!=0;
            D3D12_RANGE none{0,0}; changedReadback->Unmap(0,&none);
            ++diag.inputComparisons;
            if (changed) { same=false; reset=true; }
            descriptorCursor=0;
        }
        const bool outletChanged=settings.outletKey() != previousSettings.outletKey();
        const bool activeNrChanged=settings.firstDirty(previousSettings) < std::min(settings.passes,previousSettings.passes);
        if (same && hasNeural && !outletChanged && !activeNrChanged && settings.passes == previousSettings.passes && !reset && !forceReset) {
            if (realtime) { ++diag.cacheHits; return display(destination,key,now); }
            hr(context11->Wait(outputFence11.Get(),submitted),"OBS wait for cached output");
            context11->CopyResource(destination,sharedOutput11.Get()); ++diag.cacheHits;
            obsDrainValue=++inputValue;
            hr(context11->Signal(inputFence11.Get(),obsDrainValue),"Signal cached OBS output copy");
            context11->Flush();
            diag.state="processing"; diag.cpuSubmitMs=milliseconds(start); return true;
        }
        if (realtime) {
            if (displayFrozen && displayCycle==key.cycle && writeSlot==displaySlot) {
                // Verification and outlet changes still need to make progress
                // in a frozen preview cycle. Preserve its displayed slot and
                // supersede the other completed slot before recording a writer.
                readySlot=displaySlot; writeSlot=1-displaySlot;
            }
            if (inputFence->GetCompletedValue()<slotReadFence[writeSlot]) return hold(destination,key,now);
        }
        reset=reset || forceReset || (!same && !temporalPair(previousTime,now,false)) ||
            (hasInput && (key.inputRevision != lastKey.inputRevision || key.generation!=lastKey.generation));
        const int first=same && hasNeural && !reset ? settings.firstDirty(previousSettings) : 0;
        std::unique_lock<std::timed_mutex> execution;
        if (ngx && first<settings.passes) {
            execution=std::unique_lock<std::timed_mutex>(ngx->execution,std::defer_lock);
            // Acquire before any recording/NVOF submission so Busy cannot leave
            // a partial job. Cached outlet composition does not call NGX.
            if (!execution.try_lock_for(std::chrono::milliseconds(1)))
                throw Busy("Shared NGX session submission budget exhausted");
        }
        if (reset) nvResetPending=true;
        if (reset || outletChanged || activeNrChanged || settings.passes != previousSettings.passes) hasHistory=false;
        if (reset) { for (auto& p : passes) p.reset=true; ++diag.resets; }
        if (activeNrChanged) for (int i=settings.firstDirty(previousSettings);i<settings.passes;++i) passes[i].reset=true;
        if (settings.passes>previousSettings.passes)
            for (int i=previousSettings.passes;i<settings.passes;++i) passes[i].reset=true;
        bool recordedConstantZeros=false;
        auto* command=begin(0); timestamp(command,0);
        bool motionValid=!same && hasInput && !reset && temporalPair(previousTime,now,false) &&
                         flow->method() && flow->readyForMotion();
        auto params=parameters(settings,now,hasHistory,motionValid);
        if (mode==BackendTestMode::ControlledDelta) params.testDelta=(key.cycle&1) ? .03f : .02f;
        if (!same || !hasNeural || reset) {
            context11->CopyResource(sharedInput11.Get(),original);
            hr(context11->Signal(inputFence11.Get(),++inputValue),"OBS signal input copy");
            context11->Flush();
            hr(queue->Wait(inputFence.Get(),inputValue),"D3D12 wait for OBS input copy");
            input.state=D3D12_RESOURCE_STATE_COMMON;
            dispatch(command,Proxy,params,inference,{&input},{&proxy});
            // These guidance textures are immutable for this resource lifetime.
            // Commit the flag only after the command list is submitted.
            if (!constantZerosInitialized) {
                dispatch(command,Zero,params,inference,{}, {nullptr,nullptr,&zeroMotion,&depth});
                recordedConstantZeros=true;
            }
            if (!motionValid && params.af!=0) dispatch(command,Zero,params,inference,{}, {nullptr,nullptr,&motion,nullptr});
        }
        timestamp(command,1); timestamp(command,2);
        if ((!same || (reset && flow->readyForMotion())) && flow->method()) {
            if (flow->method() == 1) {
                dispatch(command,FlowColor,params,flow->colorExtent(),{&proxy},{&flowColor});
                transition(command,flowColor,D3D12_RESOURCE_STATE_COMMON);
                flow->recordAmd(command,flowColor.resource.Get(),reset || !hasInput);
            } else if (motionValid) {
                dispatch(command,FlowLuma,params,flow->colorExtent(),{&proxy},{nullptr,nullptr,nullptr,&flowColor});
                dispatch(command,FlowLuma,params,flow->colorExtent(),{&previousProxy},{nullptr,nullptr,nullptr,&previousFlowColor});
                transition(command,flowColor,D3D12_RESOURCE_STATE_COMMON);
                transition(command,previousFlowColor,D3D12_RESOURCE_STATE_COMMON);
                submit(0);
                flow->submitNv(flowColor.resource.Get(),previousFlowColor.resource.Get(),
                    flowColor11.Get(),previousFlowColor11.Get(),completed.Get(),submitted,nvResetPending);
                nvResetPending=false;
                hr(queue->Wait(flow->fence(),flow->fenceValue()),"Wait NVOF completion");
                command=begin(1);
            }
            if (motionValid) {
                Texture vectors; vectors.resource=flow->vectors(); vectors.extent=flow->vectorExtent();
                vectors.format=DXGI_FORMAT_R16G16_SINT;
                dispatch(command,Dense,params,inference,{nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&vectors},
                    {nullptr,nullptr,&motion});
                transition(command,vectors,D3D12_RESOURCE_STATE_COMMON);
            }
        }
        timestamp(command,3); timestamp(command,4);
        for (int i=first;i<settings.passes;++i) {
            auto& p=passes[i];
            if (ngx && !p.handle) {
                const auto featureStart=Clock::now();
                ngx->create(command,inference,&p.parameters,&p.handle,settings.modelPreset); p.reset=true;
                diag.featureCreateMs+=milliseconds(featureStart);
            }
            Texture& previous=i ? passes[i-1].output : proxy;
            transition(command,previous,D3D12_RESOURCE_STATE_COMMON);
            transition(command,p.output,D3D12_RESOURCE_STATE_COMMON);
            auto& nrMotion=settings.flow && motionValid ? motion : zeroMotion;
            transition(command,nrMotion,D3D12_RESOURCE_STATE_COMMON);
            transition(command,depth,D3D12_RESOURCE_STATE_COMMON);
            if (ngx) ngx->evaluate(command,p.parameters,p.handle,previous.resource.Get(),p.output.resource.Get(),
                nrMotion.resource.Get(),depth.resource.Get(),inference,settings.pass[i],reset || p.reset || (same && i >= first));
            else dispatch(command,ModelTest,params,inference,{&previous},{&p.output});
            p.output.state=ngx ? D3D12_RESOURCE_STATE_COMMON : p.output.state;
            ++diag.evaluations; p.reset=false;
            preparedPasses=std::max(preparedPasses,i+1);
        }
        if (execution.owns_lock()) execution.unlock();
        timestamp(command,5); timestamp(command,6);
        if (first<settings.passes) ++diag.fullUpdates;
        dispatch(command,Delta,params,inference,{&proxy,&passes[settings.passes-1].output},{&reducedDelta,&metrics});
        timestamp(command,7); timestamp(command,8);
        Texture* displayDelta=&reducedDelta;
        if (params.af!=0 || sourceExtent!=inference) {
            dispatch(command,Temporal,params,sourceExtent,{&reducedDelta,&previousDelta,&input,&previousInput,&motion},{&stable});
            displayDelta=&stable;
        }
        timestamp(command,9); timestamp(command,10);
        dispatch(command,Compose,params,sourceExtent,{&input,displayDelta,&metrics},{&composite});
        auto& targetOutput=frameOutput(realtime ? writeSlot : 0);
        copy(command,targetOutput,composite); transition(command,targetOutput,D3D12_RESOURCE_STATE_COMMON);
        // Commit history only for a newly accepted input or a reset revision. Never
        // feed temporal composite back into the neural image chain.
        if (!same || !hasNeural || !hasHistory) {
            if (params.af!=0) copy(command,previousDelta,stable);
            copy(command,previousInput,input);
            if (flow->method()) copy(command,previousProxy,proxy);
            hasHistory=true;
            if (!same || !hasNeural) ++diag.temporalCommits;
        }
        transition(command,input,D3D12_RESOURCE_STATE_COMMON);
        snapshotWrite=-1;
        if (settings.processingMode && first<settings.passes) publishSnapshot(command,settings,key,now);
        timestamp(command,11);
        command->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,12,timingReadback.Get(),0);
        const int index=command == list[1].Get() ? 1 : 0;
        if (realtime && testDelayMs) {
            if (!testDelayFence) hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&testDelayFence)),"Create controlled delay fence");
            if (testDelayWorker.joinable()) testDelayWorker.join();
            const auto value=++testDelayValue;
            hr(queue->Wait(testDelayFence.Get(),value),"Queue controlled GPU delay");
            testDelayWorker=std::jthread([fence=testDelayFence,value,delay=testDelayMs] { Sleep(delay); fence->Signal(value); });
        }
        submit(index); timingsPending=true;
        if (recordedConstantZeros) constantZerosInitialized=true;
        if (snapshotWrite>=0) snapshots[snapshotWrite].fence=submitted;
        if (realtime) {
            pending=Pending::Frame;
            slotTime[writeSlot]=now; slotFrame[writeSlot]=++outputSerial;
            slotKey[writeSlot]=key; slotSettings[writeSlot]=settings;
            slotInvalidateDisplay[writeSlot]=(key==lastKey && (reset || outletChanged || activeNrChanged || settings.passes!=previousSettings.passes));
            resetPending=false;
        } else {
            if (snapshotWrite>=0) {
                // Immediate probes already synchronize this result on D3D11;
                // the light queue must use the GPU fence, never CPU assumption.
                snapshots[snapshotWrite].valid=true; snapshotWrite=-1;
            }
            hr(context11->Wait(outputFence11.Get(),submitted),"OBS wait for complete neural frame");
            context11->CopyResource(destination,sharedOutput11.Get());
            obsDrainValue=++inputValue;
            hr(context11->Signal(inputFence11.Get(),obsDrainValue),"Signal OBS output copy");
            context11->Flush();
        }
        if (!same) { ++diag.frames; previousTime=now; }
        else ++diag.cacheHits;
        diag.flowValid=motionValid; diag.nrMotionValid=settings.flow && motionValid;
        diag.cpuSubmitMs=milliseconds(start); diag.state="processing"; diag.error.clear();
        lastKey=key; previousSettings=settings; hasInput=hasNeural=true; forceReset=false;
        return realtime ? display(destination,key,now) : true;
    }
};
struct GpuBackend::Preparation {
    struct Key {
        ID3D11Device* host=nullptr;
        Extent source,inference;
        std::string runtime;
        int model=1,passes=1,mode=0,flow=0,quality=0,afFamily=0;
        bool operator==(const Key&) const = default;
    };
    struct Request {
        Key key;
        Settings settings;
        ComPtr<ID3D11Device> host;
        ComPtr<ID3D11DeviceContext4> context;
        ComPtr<ID3D12Device> device;
        std::shared_ptr<NgxSession> session;
        BackendTestMode mode=BackendTestMode::Neural;
        BackendScheduling scheduling=BackendScheduling::CompletedFrames;
        unsigned budget=3,testDelay=0;
        std::uint64_t serial=0;
    };
    mutable std::mutex mutex;
    std::optional<Request> latest;
    std::optional<Key> desired;
    std::unique_ptr<Impl> ready;
    std::atomic<std::uint64_t> serial=0;
    std::uint64_t readySerial=0;
    bool working=false,claimed=false,closed=false;
    std::atomic<unsigned> retiring=0;
    std::string error;
    static Key key(ID3D11Device* host,Extent source,const Settings& s) {
        return {host,source,inferenceExtent(source,s),s.runtime,s.modelPreset,s.passes,s.processingMode,
            s.flow,s.flow==1 ? s.amdQuality : s.flow==2 ? s.nvQuality : 0,s.flow==0 && s.af>=2 ? 1 : 0};
    }
};
namespace {
void pinModule() noexcept {
    HMODULE ignored=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                      reinterpret_cast<LPCWSTR>(&pinModule),&ignored);
}
using Preparation=GpuBackend::Preparation;
using BackendImpl=GpuBackend::Impl;
// This module-owned service deliberately has no static destructor or shutdown
// join. CPU preparation and fence-retired destruction never run on OBS render.
struct Background {
    struct Retired { std::unique_ptr<BackendImpl> value; std::shared_ptr<Preparation> owner; };
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<Preparation>> owners;
    std::vector<Retired> retired;
    std::vector<std::shared_ptr<NgxSession>> sessions;
    void run() {
        for (;;) {
            std::vector<Retired> garbage;
            std::vector<std::shared_ptr<NgxSession>> oldSessions;
            std::shared_ptr<Preparation> owner;
            std::optional<Preparation::Request> request;
            std::vector<Retired> abandoned;
            {
                std::unique_lock lock(mutex);
                for (auto i=retired.begin();i!=retired.end();) {
                    if (!i->value->inFlight()) { garbage.push_back(std::move(*i)); i=retired.erase(i); }
                    else ++i;
                }
                oldSessions.swap(sessions);
                for (auto i=owners.begin();i!=owners.end();) {
                    auto state=*i;
                    std::scoped_lock stateLock(state->mutex);
                    if (state->ready && (state->closed || !state->desired || state->latest)) {
                        ++state->retiring; abandoned.push_back({std::move(state->ready),state});
                    }
                    if (state->closed && !state->working && !state->claimed && !state->ready && state->retiring.load()==0) {
                        i=owners.erase(i); continue;
                    }
                    if (!owner && !state->closed && !state->working && !state->claimed && !state->ready &&
                        state->retiring.load()==0 && state->latest) {
                        owner=state; request=std::move(state->latest); state->latest.reset(); state->working=true;
                    }
                    ++i;
                }
                if (!owner && garbage.empty() && abandoned.empty() && oldSessions.empty()) {
                    if (retired.empty()) wake.wait(lock);
                    else wake.wait_for(lock,std::chrono::milliseconds(10));
                    continue;
                }
            }
            // Do not hold service/mailbox/NGX gates during destruction or join.
            for (auto& item : abandoned) { item.value.reset(); --item.owner->retiring; }
            oldSessions.clear();
            for (auto& item : garbage) {
                item.value.reset(); if (item.owner) --item.owner->retiring;
            }
            if (!owner) continue;
            std::unique_ptr<BackendImpl> candidate;
            std::string error;
            try {
                candidate=std::make_unique<BackendImpl>(request->mode,request->budget,request->scheduling);
                candidate->testDelayMs=request->testDelay;
                candidate->prepare(request->host.Get(),request->key.source,request->settings,
                    request->session,request->device.Get(),request->context.Get(),
                    [owner,serial=request->serial] { return owner->serial.load()!=serial; });
            } catch (const std::exception& e) { error=e.what(); }
            bool publish=false;
            {
                std::scoped_lock lock(owner->mutex);
                publish=error.empty() && !owner->closed && owner->serial==request->serial;
                if (publish) {
                    owner->ready=std::move(candidate); owner->readySerial=request->serial; owner->working=false;
                }
                else if (!owner->closed && owner->serial==request->serial) owner->error=error;
            }
            if (candidate && candidate->inFlight()) {
                ++owner->retiring;
                std::scoped_lock lock(mutex); retired.push_back({std::move(candidate),owner});
            }
            // A stale, completed candidate is destroyed here on the worker.
            candidate.reset();
            if (!publish) { std::scoped_lock lock(owner->mutex); owner->working=false; }
        }
    }
};
std::atomic<Background*> backgroundInstance=nullptr;
Background& background() {
    static auto* instance=[] {
        pinModule(); auto* value=new Background;
        backgroundInstance.store(value);
        std::thread([value] { value->run(); }).detach();
        return value;
    }();
    return *instance;
}
void notifyBackground() {
    if (auto* service=backgroundInstance.load()) {
        // Pair state-only mailbox changes with the service's wait mutex so a
        // notification cannot be lost between its scan and indefinite wait.
        std::scoped_lock lock(service->mutex); service->wake.notify_one();
    }
}
std::shared_ptr<Preparation> preparation() {
    auto state=std::make_shared<Preparation>(); auto& service=background();
    { std::scoped_lock lock(service.mutex); service.owners.push_back(state); }
    return state;
}
void retire(std::unique_ptr<BackendImpl> value,const std::shared_ptr<Preparation>& owner={}) {
    if (!value) return;
    if (owner) ++owner->retiring;
    auto& service=background();
    { std::scoped_lock lock(service.mutex); service.retired.push_back({std::move(value),owner}); }
    service.wake.notify_one();
}
void retireSession(std::shared_ptr<NgxSession> value) {
    if (!value) return;
    auto& service=background();
    { std::scoped_lock lock(service.mutex); service.sessions.push_back(std::move(value)); }
    service.wake.notify_one();
}
void cancelPreparation(const std::shared_ptr<Preparation>& state,bool close=false) {
    if (!state) return;
    { std::scoped_lock lock(state->mutex);
      if (!state->desired && !state->latest && (!close || state->closed)) return;
      ++state->serial; state->desired.reset(); state->latest.reset();
      state->closed=state->closed || close; state->error.clear(); }
    notifyBackground();
}
bool preparationIdle(const std::shared_ptr<Preparation>& state) {
    if (!state) return true;
    std::scoped_lock lock(state->mutex);
    return !state->working && !state->ready && !state->claimed && !state->latest && state->retiring.load()==0;
}
void releaseClaim(const std::shared_ptr<Preparation>& state) {
    if (state) { std::scoped_lock lock(state->mutex); state->claimed=false; }
    notifyBackground();
}
void requestPreparation(const std::shared_ptr<Preparation>& state,Preparation::Request request) {
    { std::scoped_lock lock(state->mutex);
      if (state->closed || (state->desired && *state->desired==request.key)) return;
      state->desired=request.key; request.serial=++state->serial; state->latest=std::move(request); state->error.clear(); }
    notifyBackground();
}
std::unique_ptr<BackendImpl> takePrepared(const std::shared_ptr<Preparation>& state,const Preparation::Key& key) {
    std::scoped_lock lock(state->mutex);
    if (!state->ready || state->readySerial!=state->serial || !state->desired || *state->desired!=key) return {};
    state->claimed=true; return std::move(state->ready);
}
bool flowChanged(const Settings& a,const Settings& b) {
    return a.flow!=b.flow || (a.flow==1 && a.amdQuality!=b.amdQuality) ||
        (a.flow==2 && a.nvQuality!=b.nvQuality) || (a.flow==0 && (a.af>=2)!=(b.af>=2));
}
}
GpuBackend::GpuBackend(BackendTestMode mode,unsigned budget,BackendScheduling scheduling) :
    impl_(std::make_unique<Impl>(mode,std::clamp(budget,1u,3000u),scheduling)),mode_(mode),
    waitBudgetMs_(std::clamp(budget,1u,3000u)),scheduling_(scheduling) {}
GpuBackend::~GpuBackend() {
    cancelPreparation(preparation_,true);
    retire(std::move(warming_),preparation_); releaseClaim(preparation_);
    retire(std::move(recovery_),preparation_);
    retire(std::move(impl_),preparation_);
    retireSession(std::move(sessionAnchor_));
}
void GpuBackend::collectRetired() noexcept {
    // The service sleeps without work and polls only GPU-backed retirements.
}
bool GpuBackend::process(ID3D11Device* device,ID3D11Texture2D* input,ID3D11Texture2D* output,
                         const Settings& settings,FrameKey key,std::uint64_t time,bool resetRequested) {
    collectRetired();
    if (!impl_->initialized && preparation_ && !preparationIdle(preparation_)) {
        cancelPreparation(preparation_);
        impl_->diag.state="initializing"; impl_->diag.outputAvailable=false;
        impl_->diag.adaptiveReason="finishing previous resource preparation"; return false;
    }
    Settings effective=settings;
    const bool automatic=settings.scaling && settings.resolutionControl==0;
    const bool structuralEdit=impl_->initialized && (
        key.extent!=impl_->sourceExtent || device!=impl_->device11.Get() ||
        effective.runtime!=impl_->preparedSettings.runtime ||
        effective.modelPreset!=impl_->preparedSettings.modelPreset ||
        flowChanged(effective,impl_->preparedSettings) ||
        effective.passes!=impl_->preparedSettings.passes ||
        effective.processingMode!=impl_->lastProcessingMode ||
        (effective.processingMode==1 && settings.updateInterval!=impl_->previousSettings.updateInterval));
    if (structuralEdit) {
        impl_->adaptive.invalidateMeasurements("structural change");
        if (recovery_) retire(std::move(recovery_),preparation_);
    }
    if (!automatic && recovery_) retire(std::move(recovery_),preparation_);
    if (automatic) {
        const int minimum=detail::safeMinimumPercent(key.extent);
        impl_->adaptive.percent=std::clamp(impl_->adaptive.percent,minimum,100);
        impl_->adaptive.proposal=std::clamp(impl_->adaptive.proposal,minimum,100);
        effective.resolutionControl=1; effective.percent=impl_->adaptive.proposal;
    }
    auto sameResources=[&](const Impl& value,bool candidate=false) {
        if (key.extent!=value.sourceExtent || inferenceExtent(key.extent,effective)!=value.inference ||
            effective.runtime!=value.preparedSettings.runtime || effective.modelPreset!=value.preparedSettings.modelPreset ||
            (value.device11 && device!=value.device11.Get())) return false;
        if (!value.realtime) return true;
        return !flowChanged(effective,value.preparedSettings) &&
            (value.failed || effective.passes<=((candidate || !value.hasNeural) ? value.preparedSettings.passes : value.preparedPasses)) &&
            (value.failed || !effective.processingMode || (candidate ? value.preparedSettings.processingMode!=0 : value.lightweight!=nullptr));
    };
    const bool rebuild=(impl_->initialized || impl_->failed) && !sameResources(*impl_);
    if (warming_ && (!rebuild || !sameResources(*warming_,true))) {
        retire(std::move(warming_),preparation_); releaseClaim(preparation_);
    }
    if (!rebuild && preparation_) cancelPreparation(preparation_);
    std::string preparationReason;
    if (rebuild) {
        const bool asynchronous=impl_->realtime && impl_->initialized && !impl_->failed &&
            device==impl_->device11.Get() && effective.runtime==impl_->preparedSettings.runtime;
        bool canInitialize=true;
        if (asynchronous) {
            if (!warming_ && automatic && recovery_ && !recovery_->failed &&
                sameResources(*recovery_,true) && device==recovery_->device11.Get() &&
                effective.runtime==recovery_->preparedSettings.runtime) {
                cancelPreparation(preparation_);
                warming_=std::move(recovery_);
                warming_->forceReset=true; warming_->hasHistory=false; warming_->resetPending=true;
                warming_->ready=false; warming_->displayFrozen=false;
                if (warming_->lightweight) {
                    warming_->lightweight->forceReset=true; warming_->lightweight->hasHistory=false;
                    warming_->lightweight->ready=false; warming_->lightweight->displayFrozen=false;
                }
                ++warming_->clock.epoch;
                warmBaseline_=impl_->diag;
                const auto keptBytes=warming_->diag.allocatedBytes;
                warming_->diag=warmBaseline_;
                warming_->diag.allocatedBytes=keptBytes;
                warming_->adaptive=impl_->adaptive;
                warming_->diag.state="initializing";
            } else {
                if (!preparation_) preparation_=preparation();
                Preparation::Request request;
                request.key=Preparation::key(device,key.extent,effective); request.settings=effective;
                request.host=device; request.context=impl_->context11; request.device=impl_->device; request.session=impl_->ngx;
                request.mode=mode_; request.scheduling=scheduling_; request.budget=waitBudgetMs_; request.testDelay=impl_->testDelayMs;
                requestPreparation(preparation_,std::move(request));
                if (!warming_) {
                    warming_=takePrepared(preparation_,Preparation::key(device,key.extent,effective));
                    if (warming_) {
                        warmBaseline_=impl_->diag;
#define INHERIT_COUNTER(name) warming_->diag.name+=warmBaseline_.name
                        INHERIT_COUNTER(frames); INHERIT_COUNTER(evaluations); INHERIT_COUNTER(cacheHits); INHERIT_COUNTER(resets);
                        INHERIT_COUNTER(passthrough); INHERIT_COUNTER(temporalCommits); INHERIT_COUNTER(inputComparisons);
                        INHERIT_COUNTER(renderRequests); INHERIT_COUNTER(completedFrames); INHERIT_COUNTER(displays); INHERIT_COUNTER(busy);
                        INHERIT_COUNTER(droppedCandidates); INHERIT_COUNTER(fullUpdates); INHERIT_COUNTER(lightweightComposites);
#undef INHERIT_COUNTER
                        warming_->adaptive=impl_->adaptive;
                        if (warming_->diag.firstError.empty()) warming_->diag.firstError=impl_->diag.firstError;
                    }
                }
            }
            if (preparation_) { std::scoped_lock stateLock(preparation_->mutex);
              preparationReason=preparation_->error.empty() ? "preparing requested resources; showing current completed output" :
                  "preparation failed; keeping completed output: "+preparation_->error; }
            else preparationReason="warming standby tier; showing current completed output";
        } else {
            // Runtime/host-device changes keep their existing synchronous NGX
            // initialization path. First drain/cancel this owner's preparation;
            // do not introduce parallel core Init/Shutdown across runtimes.
            cancelPreparation(preparation_);
            canInitialize=preparationIdle(preparation_);
            if (!warming_ && canInitialize) {
                warming_=std::make_unique<Impl>(mode_,waitBudgetMs_,scheduling_);
                warmBaseline_=impl_->diag; warming_->diag=warmBaseline_; warming_->diag.allocatedBytes=0;
                warming_->sourceExtent=key.extent; warming_->inference=inferenceExtent(key.extent,effective);
                warming_->previousSettings=warming_->preparedSettings=effective; warming_->adaptive=impl_->adaptive;
                warming_->testDelayMs=impl_->testDelayMs; warming_->diag.state="initializing";
            }
            preparationReason=canInitialize ? "initializing selected runtime" : "finishing resource preparation";
        }
        bool candidateReady=false;
        if (warming_ && !warming_->failed) {
            try {
                candidateReady=warming_->process(device,input,output,effective,key,time,resetRequested);
                candidateReady=candidateReady && (!warming_->realtime || warming_->ready);
                if (candidateReady && warming_->realtime && effective.processingMode) {
                    const auto& light=warming_->lightweight;
                    candidateReady=light && light->diag.outputAvailable && light->slotHasResidual[light->displaySlot];
                }
            } catch (const Busy&) { ++warming_->diag.busy; }
            catch (const std::exception& e) {
                warming_->failed=true; warming_->diag.error=e.what();
                if (warming_->diag.firstError.empty()) warming_->diag.firstError=e.what();
                writeDiagnosticLog(std::string("Replacement backend: ")+e.what());
            }
        }
        const bool frozen=impl_->realtime && ((impl_->lightweight && impl_->lightweight->displayFrozen &&
            impl_->lightweight->displayCycle==key.cycle) || (impl_->displayFrozen && impl_->displayCycle==key.cycle));
        if (candidateReady && !frozen) {
            // Counters stay cumulative while allocation ownership and histories
            // change atomically. Retired instances keep every GPU reader alive.
#define MERGE_COUNTER(name) warming_->diag.name+=impl_->diag.name-warmBaseline_.name
            MERGE_COUNTER(frames); MERGE_COUNTER(evaluations); MERGE_COUNTER(cacheHits); MERGE_COUNTER(resets);
            MERGE_COUNTER(passthrough); MERGE_COUNTER(temporalCommits); MERGE_COUNTER(inputComparisons);
            MERGE_COUNTER(renderRequests); MERGE_COUNTER(completedFrames); MERGE_COUNTER(displays); MERGE_COUNTER(busy);
            MERGE_COUNTER(droppedCandidates); MERGE_COUNTER(fullUpdates); MERGE_COUNTER(lightweightComposites);
#undef MERGE_COUNTER
            if (automatic) warming_->adaptive.switched(effective.percent);
            else warming_->adaptive.reason="manual resolution";
            const bool keepRecovery=automatic && impl_->initialized && !impl_->failed &&
                key.extent==impl_->sourceExtent &&
                effective.runtime==impl_->preparedSettings.runtime &&
                effective.modelPreset==impl_->preparedSettings.modelPreset &&
                !flowChanged(effective,impl_->preparedSettings) &&
                effective.passes==impl_->preparedSettings.passes &&
                effective.processingMode==impl_->lastProcessingMode;
            if (keepRecovery) {
                if (recovery_) retire(std::move(recovery_),preparation_);
                recovery_=std::move(impl_);
            } else {
                retire(std::move(impl_),preparation_);
            }
            impl_=std::move(warming_); releaseClaim(preparation_);
            if (impl_->ngx && SUCCEEDED(impl_->ngx->device->GetDeviceRemovedReason())) {
                if (sessionAnchor_!=impl_->ngx) retireSession(std::move(sessionAnchor_));
                sessionAnchor_=impl_->ngx;
            }
            impl_->diag.adaptiveReason="requested resources active; "+impl_->adaptive.reason;
            return true;
        }
        // An invalid explicit runtime selection keeps its established visible
        // passthrough behavior; the healthy instance remains available to retry.
        if (warming_ && warming_->failed && effective.runtime!=impl_->previousSettings.runtime) {
            impl_->diag.error=warming_->diag.error;
            if (impl_->diag.firstError.empty()) impl_->diag.firstError=warming_->diag.error;
            impl_->diag.state="passthrough"; ++impl_->diag.passthrough; return false;
        }
        if (warming_ && !warming_->failed) preparationReason=candidateReady ?
            "requested result ready; waiting for next video cycle; showing current completed output" :
            "resources prepared; waiting for first requested result; showing current completed output";
        if (key.extent!=impl_->sourceExtent || device!=impl_->device11.Get()) {
            impl_->diag.outputAvailable=false; impl_->diag.state="initializing";
            impl_->diag.adaptiveReason=preparationReason; return false;
        }
        effective.percent=impl_->diag.currentPercent; effective.resolutionControl=1;
        effective.scaling=effective.percent!=100 || settings.scaling;
        effective.runtime=impl_->previousSettings.runtime; effective.modelPreset=impl_->previousSettings.modelPreset;
        effective.flow=impl_->preparedSettings.flow; effective.amdQuality=impl_->preparedSettings.amdQuality;
        effective.nvQuality=impl_->preparedSettings.nvQuality;
        if (effective.flow==0 && (effective.af>=2)!=(impl_->preparedSettings.af>=2)) effective.af=impl_->previousSettings.af;
        if (impl_->realtime) {
            effective.passes=impl_->previousSettings.passes;
            effective.processingMode=impl_->lastProcessingMode;
        }
        if (warming_ && warming_->failed) preparationReason="candidate failed; keeping completed output: "+warming_->diag.error;
        impl_->adaptive.reason=preparationReason;
    }
    std::scoped_lock lock(impl_->mutex);
    try {
        if (impl_->initialized && flowChanged(effective,impl_->previousSettings)) {
            impl_->wait(impl_->realtime ? 0 : waitBudgetMs_); impl_->configureFlow(effective); resetRequested=true;
            if (impl_->lightweight) {
                impl_->lightweight->wait(impl_->realtime ? 0 : waitBudgetMs_);
                impl_->lightweight->configureFlow(effective); impl_->lightweight->forceReset=true;
            }
        }
        const bool accepted=impl_->process(device,input,output,effective,key,time,resetRequested);
        if (impl_->ngx && SUCCEEDED(impl_->ngx->device->GetDeviceRemovedReason())) {
            if (sessionAnchor_!=impl_->ngx) retireSession(std::move(sessionAnchor_));
            sessionAnchor_=impl_->ngx;
        }
        if (!rebuild && !warming_) {
            const double scalableFullMs=impl_->diag.gpuPrepareMs+impl_->diag.gpuNrMs+impl_->diag.gpuPostMs;
            const double fixedFullMs=std::max(0.0,impl_->diag.gpuTotalMs-scalableFullMs);
            impl_->adaptive.observe(key.cycle,key.extent,settings,impl_->diag.completedFrames,
                impl_->diag.gpuTotalMs,impl_->lightweight ? impl_->lightweight->diag.gpuTotalMs : 0,
                impl_->diag.droppedCandidates,impl_->diag.outputAgeMs,
                scalableFullMs,fixedFullMs);
        }
        impl_->diag.adaptiveReason=rebuild ? preparationReason : "requested resources active; "+impl_->adaptive.reason;
        impl_->diag.modelPreset=settings.modelPreset;
        return accepted;
    } catch (const Busy&) {
        if (impl_->realtime) {
            impl_->resetPending=impl_->resetPending || resetRequested;
            impl_->diag.error.clear();
            return impl_->hold(output,key,time);
        }
        impl_->diag.state="busy"; impl_->diag.error="GPU submission is still pending";
        ++impl_->diag.busy; ++impl_->diag.passthrough; return false;
    } catch (const std::exception& e) {
        impl_->diag.error=e.what();
        if (impl_->device) {
            const HRESULT removed=impl_->device->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                std::ostringstream reason; reason << "; deviceRemovedReason=0x" << std::hex << static_cast<unsigned>(removed);
                impl_->diag.error+=reason.str();
                Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
                if (SUCCEEDED(impl_->device.As(&dred))) {
                    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
                    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
                        for (auto* node=breadcrumbs.pHeadAutoBreadcrumbNode;node;node=node->pNext) {
                            if (node->pLastBreadcrumbValue && *node->pLastBreadcrumbValue<node->BreadcrumbCount) {
                                const auto last=*node->pLastBreadcrumbValue;
                                impl_->diag.error+="; DRED="+std::to_string(last)+"/"+std::to_string(node->BreadcrumbCount);
                                if (node->pCommandListDebugNameA) impl_->diag.error+=" list="+std::string(node->pCommandListDebugNameA);
                                if (last<node->BreadcrumbCount) impl_->diag.error+=" op="+std::to_string(node->pCommandHistory[last]);
                            }
                        }
                    }
                    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
                    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA)
                        impl_->diag.error+="; DRED pageFaultVA="+std::to_string(fault.PageFaultVA);
                }
            }
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
            if (SUCCEEDED(impl_->device.As(&messages))) {
                const auto total=messages->GetNumStoredMessagesAllowedByRetrievalFilter();
                for (UINT64 i=total>10 ? total-10 : 0;i<total;++i) {
                    SIZE_T size=0; messages->GetMessage(i,nullptr,&size);
                    std::vector<unsigned char> bytes(size);
                    auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                    if (SUCCEEDED(messages->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)
                        impl_->diag.error+="; D3D12="+std::string(message->pDescription);
                }
            }
        }
        if (impl_->diag.firstError.empty()) impl_->diag.firstError=e.what();
        impl_->diag.state="passthrough"; ++impl_->diag.passthrough;
        // Typed Busy is handled above. Native/feature errors invalidate output
        // and require explicit or configuration-driven reinitialization.
        impl_->failed=true;
        impl_->hasHistory=false;
        impl_->forceReset=true; impl_->hasNeural=false; impl_->ready=false;
        writeDiagnosticLog(impl_->diag.error); return false;
    }
}
void GpuBackend::reset() noexcept {
    std::scoped_lock lock(impl_->mutex); impl_->forceReset=true; impl_->hasHistory=false;
    ++impl_->clock.epoch;
    if (impl_->lightweight) { impl_->lightweight->forceReset=true; impl_->lightweight->hasHistory=false; }
    if (warming_) { warming_->forceReset=true; warming_->hasHistory=false; ++warming_->clock.epoch; }
}
void GpuBackend::setTestDelay(unsigned delay) {
    if (mode_!=BackendTestMode::ControlledDelta || scheduling_!=BackendScheduling::CompletedFrames)
        throw std::runtime_error("Controlled delay requires the realtime test backend");
    std::scoped_lock lock(impl_->mutex); impl_->testDelayMs=std::min(delay,200u);
}
void GpuBackend::retry() noexcept {
    try {
        if (!preparation_) preparation_=preparation();
        cancelPreparation(preparation_);
        if (preparation_ && preparation_->retiring.load()>=2) {
            impl_->diag.error="GPU retirement limit reached; wait for completion or restart OBS"; return;
        }
        retire(std::move(warming_),preparation_); releaseClaim(preparation_);
        retire(std::move(recovery_),preparation_);
        const auto previous=impl_->diag;
        retire(std::move(impl_),preparation_);
        impl_=std::make_unique<Impl>(mode_,waitBudgetMs_,scheduling_); impl_->diag=previous;
        impl_->diag.error.clear(); impl_->diag.allocatedBytes=0;
        impl_->diag.state="initializing"; impl_->diag.source={}; impl_->diag.inference={};
    } catch (...) {}
}
Diagnostics GpuBackend::diagnostics() const {
    std::scoped_lock lock(impl_->mutex); auto result=impl_->diag;
    result.preparingResources=bool(warming_) || !preparationIdle(preparation_);
    if (impl_->lightweight) result.allocatedBytes+=impl_->lightweight->diag.allocatedBytes;
    if (warming_) {
        result.allocatedBytes+=warming_->diag.allocatedBytes;
        if (warming_->lightweight) result.allocatedBytes+=warming_->lightweight->diag.allocatedBytes;
    }
    return result;
}
std::vector<std::array<float,2>> GpuBackend::inspectMotion() {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->initialized) throw std::runtime_error("Motion probe needs an initialized GPU backend");
    impl_->wait(3000); impl_->harvest();
    auto desc=impl_->motion.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes=0;
    impl_->device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=bytes; buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
    buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    hr(impl_->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&readback)),"Allocate offline motion probe");
    auto* command=impl_->begin(0);
    transition(command,impl_->motion,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint=footprint;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=impl_->motion.resource.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    impl_->submit(0); impl_->wait(3000);
    void* mapped=nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
    hr(readback->Map(0,&range,&mapped),"Map offline motion probe");
    std::vector<std::array<float,2>> result(static_cast<size_t>(impl_->inference.width)*impl_->inference.height);
    for (unsigned y=0;y<impl_->inference.height;++y)
        std::memcpy(result.data()+static_cast<size_t>(y)*impl_->inference.width,
            static_cast<unsigned char*>(mapped)+static_cast<size_t>(y)*footprint.Footprint.RowPitch,impl_->inference.width*8);
    D3D12_RANGE none{0,0}; readback->Unmap(0,&none);
    return result;
}
}
