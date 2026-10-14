// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Settings.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <memory>
#include <vector>

namespace neural {
struct NgxSession;
std::string resolveRuntimePath(const std::string& selection);
void warmShaderBytecode();
struct Diagnostics {
    std::string state = "waiting", error, firstError, identity, flow = "none";
    std::uint64_t frames = 0, evaluations = 0, cacheHits = 0, resets = 0, passthrough = 0;
    double cpuSubmitMs = 0, waitMs = 0, gpuPrepareMs = 0, gpuNrMs = 0, gpuPostMs = 0,
           gpuTemporalMs = 0, gpuTotalMs = 0;
    std::uint64_t allocatedBytes = 0;
    Extent source, inference;
    bool flowValid = false;
    bool nrMotionValid = false;
    std::uint64_t temporalCommits = 0;
    std::uint64_t inputComparisons = 0;
    std::uint64_t renderRequests=0, completedFrames=0, displays=0, busy=0, droppedCandidates=0;
    std::uint64_t submittedFence=0, completedFence=0, displayedFrame=0;
    double outputAgeMs=0, initializationMs=0, shaderCompileMs=0, featureCreateMs=0;
    bool outputAvailable=false;
    std::string report() const;
};
enum class BackendTestMode { Neural, Identity, ControlledDelta };
enum class BackendScheduling { Immediate, CompletedFrames };
class GpuBackend {
public:
    struct Impl;
    // OBS uses the default 3 ms. Offline readback probes may use a longer
    // budget so scheduler jitter does not masquerade as a pixel-contract fault.
    explicit GpuBackend(BackendTestMode mode = BackendTestMode::Neural, unsigned waitBudgetMs = 3,
                        BackendScheduling scheduling = BackendScheduling::Immediate);
    ~GpuBackend();
    GpuBackend(const GpuBackend&) = delete;
    GpuBackend& operator=(const GpuBackend&) = delete;
    // All input/output textures are same-device, linear SDR, premultiplied RGBA16F.
    bool process(ID3D11Device* device, ID3D11Texture2D* input, ID3D11Texture2D* output,
                 const Settings& settings, FrameKey key, std::uint64_t timeNs, bool reset);
    void reset() noexcept;
    void retry() noexcept;
    Diagnostics diagnostics() const;
    // Offline test/probe only. Never called from the OBS render path.
    std::vector<std::array<float,2>> inspectMotion();
    static void collectRetired() noexcept;
    // ControlledDelta test backend only; unavailable to the production filter.
    void setTestDelay(unsigned milliseconds);
private:
    std::unique_ptr<Impl> impl_;
    // Preserve the last healthy device/runtime across failed selections and
    // size rebuilds. Feature handles and textures remain per backend instance.
    std::shared_ptr<NgxSession> sessionAnchor_;
    BackendTestMode mode_;
    unsigned waitBudgetMs_;
    BackendScheduling scheduling_;
};
} // namespace neural
