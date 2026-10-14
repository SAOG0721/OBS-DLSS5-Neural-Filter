// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <chrono>
#include <limits>
#include <stdexcept>
namespace neural {
// Event handles can still be signalled by an older, timed-out registration.
// Only the fence counter proves completion of this specific submission.
inline bool waitForFence(ID3D12Fence* fence,UINT64 target,HANDLE event,unsigned timeoutMs) {
    using Clock=std::chrono::steady_clock;
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto value=fence->GetCompletedValue();
        if (value==std::numeric_limits<UINT64>::max()) throw std::runtime_error("GPU fence reports device removal");
        if (value>=target) return true;
        const auto remaining=deadline-Clock::now();
        if (remaining<=Clock::duration::zero()) return false;
        if (!ResetEvent(event)) throw std::runtime_error("Reset GPU completion event failed");
        if (FAILED(fence->SetEventOnCompletion(target,event))) throw std::runtime_error("Arm GPU completion event failed");
        const auto ms=std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        const auto result=WaitForSingleObject(event,static_cast<DWORD>(ms));
        if (result==WAIT_FAILED) throw std::runtime_error("Wait for GPU completion event failed");
        // Recheck even on timeout: completion may race the wait boundary.
        // A wake-up alone never authorizes allocator/resource reuse.
    }
}
}
