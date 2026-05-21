#pragma once

#include <d3d11.h>
#include <wrl/client.h>

// GPU-accelerated background blur using D3D11 downsample + box blur passes.
// Renders into ImGui's background draw list as a fullscreen blurred texture.
namespace BlurEffect {
    using Microsoft::WRL::ComPtr;

    // Initialize GPU resources (called once from PresentHook on first use).
    void Init(ID3D11Device* device, IDXGISwapChain* swapChain);

    // Release all GPU resources.
    void Shutdown();

    // Returns true if Init() has been called successfully.
    bool IsInitialized();

    // Capture the current back buffer, blur it, and render it as a fullscreen
    // quad into the immediate context. Call BEFORE ImGui::NewFrame().
    void RenderBlurredBackground(ID3D11DeviceContext* ctx, IDXGISwapChain* swapChain);
}
