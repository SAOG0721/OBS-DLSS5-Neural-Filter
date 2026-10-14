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
    // AMD records on the caller's list. D3D11 NVOF submits GPU work after waiting
    // on readyFence; caller queues a wait on returned completion fence.
    void initialize(ID3D12Device* device, ID3D11Device* obsDevice, Extent inference, const Settings& settings);
    void recordAmd(ID3D12GraphicsCommandList* list, ID3D12Resource* color, bool reset);
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
    std::string description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
