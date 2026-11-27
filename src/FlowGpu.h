// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Settings.h"
#include <d3d12.h>
#include <d3d11_4.h>
#include <memory>
#include <string>
namespace neural {
class FlowGpu {
public:
    FlowGpu();
    ~FlowGpu();
    // AMD records on the caller's list. A private NVOF worker waits for the
    // grayscale producer before dispatch; the caller never waits on that CPU
    // work. At most one Nv submission is outstanding, and its inputs remain
    // owned by this session through the returned completion fence.
    void initialize(ID3D12Device* device, ID3D11Device* obsDevice, Extent inference, const Settings& settings);
    void recordAmd(ID3D12GraphicsCommandList* list, ID3D12Resource* color, bool reset);
    // Register immutable input resources while preparing a fresh context;
    // subsequent render submissions reuse these registrations.
    void prepareNvInputs(ID3D12Resource* current, ID3D12Resource* previous,
                         ID3D11Texture2D* current11, ID3D11Texture2D* previous11);
    void submitNv(ID3D12Resource* current, ID3D12Resource* previous,
                  ID3D11Texture2D* current11, ID3D11Texture2D* previous11,
                  ID3D12Fence* readyFence, std::uint64_t readyValue, bool reset);
    ID3D12Resource* vectors() const noexcept;
    ID3D12Fence* fence() const noexcept;
    std::uint64_t fenceValue() const noexcept;
    Extent vectorExtent() const noexcept;
    Extent colorExtent() const noexcept;
    DXGI_FORMAT colorFormat() const noexcept;
    int grid() const noexcept;
    float divisor() const noexcept;
    int method() const noexcept;
    bool readyForMotion() const noexcept;
    void check() const;
    std::string description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
