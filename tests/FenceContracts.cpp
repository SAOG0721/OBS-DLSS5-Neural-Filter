// SPDX-License-Identifier: GPL-3.0-only
#include "FenceWait.h"
#include <wrl/client.h>
#include <thread>
#include <iostream>
using Microsoft::WRL::ComPtr;
void require(bool okay,const char* message) { if (!okay) throw std::runtime_error(message); }
int main() {
    try {
        ComPtr<ID3D12Device> device;
        require(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"Create test device");
        ComPtr<ID3D12Fence> fence;
        require(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"Create test fence");
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"Create test event");
        // Prove the old implementation is vulnerable to a timed-out target 1
        // waking a later target 2 before that submission actually completes.
        require(SUCCEEDED(fence->SetEventOnCompletion(1,event)),"Arm old event");
        require(SUCCEEDED(fence->SetEventOnCompletion(2,event)),"Arm new event");
        fence->Signal(1);
        require(WaitForSingleObject(event,1000)==WAIT_OBJECT_0 && fence->GetCompletedValue()==1,"Legacy stale wake-up did not reproduce");
        fence->Signal(2); ResetEvent(event);
        require(!neural::waitForFence(fence.Get(),3,event,2),"Pending fence incorrectly passed timeout");
        std::jthread delayed([&] { Sleep(10); fence->Signal(3); Sleep(40); fence->Signal(4); });
        require(neural::waitForFence(fence.Get(),4,event,1000),"Current target did not complete");
        require(fence->GetCompletedValue()>=4,"Old timeout event was mistaken for target 4 completion");
        delayed.join();
        require(neural::waitForFence(fence.Get(),4,event,0),"Already completed fence failed zero budget");
        require(!neural::waitForFence(fence.Get(),5,event,0),"Pending fence passed zero budget");
        fence->Signal(5); CloseHandle(event);
        std::cout << "Old-event premature completion reproduced; corrected wait verifies the requested fence value after every wake-up.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
