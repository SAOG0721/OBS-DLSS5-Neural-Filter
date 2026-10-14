// SPDX-License-Identifier: MIT
#pragma once
#include "Settings.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <string>

namespace neural {
void writeDiagnosticLog(const std::string& message) noexcept;
struct NgxSession {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::timed_mutex execution;
    std::string identity;
    static std::shared_ptr<NgxSession> acquire(IDXGIAdapter1* adapter, const std::string& runtime);
    ~NgxSession();
    void create(ID3D12GraphicsCommandList* list, Extent e, NVSDK_NGX_Parameter** parameters,
                NVSDK_NGX_Handle** handle);
    void evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters,
                  NVSDK_NGX_Handle* handle, ID3D12Resource* input, ID3D12Resource* output,
                  ID3D12Resource* motion, ID3D12Resource* depth, Extent e,
                  const PassSettings& settings, bool reset);
    void release(NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle* handle) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    NgxSession();
};
}
