#include "BlurEffect.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace BlurEffect {

    static bool initialized = false;
    static ComPtr<ID3D11Device> g_device;
    static ComPtr<ID3D11VertexShader> g_vsFullscreen;
    static ComPtr<ID3D11PixelShader> g_psDownsample;
    static ComPtr<ID3D11PixelShader> g_psCopy;
    static ComPtr<ID3D11SamplerState> g_sampler;

    // Ping-pong textures at reduced resolution for blur passes
    static constexpr int kDownscaleFactor = 4;
    static constexpr int kBlurPasses = 3;
    static UINT g_blurWidth = 0;
    static UINT g_blurHeight = 0;
    static ComPtr<ID3D11Texture2D> g_blurTex[2];
    static ComPtr<ID3D11RenderTargetView> g_blurRTV[2];
    static ComPtr<ID3D11ShaderResourceView> g_blurSRV[2];

    // SRV for reading the back buffer
    static ComPtr<ID3D11Texture2D> g_backBufferCopy;
    static ComPtr<ID3D11ShaderResourceView> g_backBufferSRV;
    static UINT g_backBufferWidth = 0;
    static UINT g_backBufferHeight = 0;

    // Constant buffer for texel size
    struct BlurCB {
        float texelSizeX;
        float texelSizeY;
        float pad[2];
    };
    static ComPtr<ID3D11Buffer> g_cbBlur;

    // Fullscreen triangle vertex shader — generates positions from vertex ID
    static const char* vsFullscreenSrc = R"(
        void main(uint id : SV_VertexID,
                  out float4 pos : SV_Position,
                  out float2 uv : TEXCOORD0) {
            uv = float2((id << 1) & 2, id & 2);
            pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
        }
    )";

    // Simple copy shader
    static const char* psCopySrc = R"(
        Texture2D tex : register(t0);
        SamplerState samp : register(s0);
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            return tex.Sample(samp, uv);
        }
    )";

    // Box blur shader (5-tap horizontal + vertical combined via bilinear sampling)
    static const char* psDownsampleSrc = R"(
        Texture2D tex : register(t0);
        SamplerState samp : register(s0);
        cbuffer CB : register(b0) {
            float2 texelSize;
        };
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float4 color = tex.Sample(samp, uv) * 0.25;
            color += tex.Sample(samp, uv + float2( texelSize.x,  texelSize.y)) * 0.1875;
            color += tex.Sample(samp, uv + float2(-texelSize.x,  texelSize.y)) * 0.1875;
            color += tex.Sample(samp, uv + float2( texelSize.x, -texelSize.y)) * 0.1875;
            color += tex.Sample(samp, uv + float2(-texelSize.x, -texelSize.y)) * 0.1875;
            return color;
        }
    )";

    static ComPtr<ID3DBlob> CompileShader(const char* src, const char* target) {
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                                "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                blob.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            if (errors) {
                logger::error("[BlurEffect] Shader compile error: {}",
                              (const char*)errors->GetBufferPointer());
            }
            return nullptr;
        }
        return blob;
    }

    static bool CreateBlurTargets() {
        for (int i = 0; i < 2; i++) {
            g_blurTex[i].Reset();
            g_blurRTV[i].Reset();
            g_blurSRV[i].Reset();

            D3D11_TEXTURE2D_DESC td{};
            td.Width = g_blurWidth;
            td.Height = g_blurHeight;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = g_device->CreateTexture2D(&td, nullptr, g_blurTex[i].GetAddressOf());
            if (FAILED(hr)) return false;

            hr = g_device->CreateRenderTargetView(g_blurTex[i].Get(), nullptr, g_blurRTV[i].GetAddressOf());
            if (FAILED(hr)) return false;

            hr = g_device->CreateShaderResourceView(g_blurTex[i].Get(), nullptr, g_blurSRV[i].GetAddressOf());
            if (FAILED(hr)) return false;
        }
        return true;
    }

    void Init(ID3D11Device* device, IDXGISwapChain* swapChain) {
        if (initialized) return;

        g_device = device;

        // Get back buffer dimensions
        ComPtr<ID3D11Texture2D> backBuffer;
        swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        D3D11_TEXTURE2D_DESC bbDesc{};
        backBuffer->GetDesc(&bbDesc);
        g_backBufferWidth = bbDesc.Width;
        g_backBufferHeight = bbDesc.Height;
        g_blurWidth = bbDesc.Width / kDownscaleFactor;
        g_blurHeight = bbDesc.Height / kDownscaleFactor;

        // Create back buffer copy texture (same size, for reading)
        D3D11_TEXTURE2D_DESC copyDesc = bbDesc;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.MiscFlags = 0;
        device->CreateTexture2D(&copyDesc, nullptr, g_backBufferCopy.GetAddressOf());
        device->CreateShaderResourceView(g_backBufferCopy.Get(), nullptr, g_backBufferSRV.GetAddressOf());

        // Create blur render targets
        if (!CreateBlurTargets()) {
            logger::error("[BlurEffect] Failed to create blur targets");
            return;
        }

        // Compile shaders
        auto vsBlob = CompileShader(vsFullscreenSrc, "vs_5_0");
        auto psCopyBlob = CompileShader(psCopySrc, "ps_5_0");
        auto psBlurBlob = CompileShader(psDownsampleSrc, "ps_5_0");
        if (!vsBlob || !psCopyBlob || !psBlurBlob) {
            logger::error("[BlurEffect] Shader compilation failed");
            return;
        }

        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   nullptr, g_vsFullscreen.GetAddressOf());
        device->CreatePixelShader(psCopyBlob->GetBufferPointer(), psCopyBlob->GetBufferSize(),
                                  nullptr, g_psCopy.GetAddressOf());
        device->CreatePixelShader(psBlurBlob->GetBufferPointer(), psBlurBlob->GetBufferSize(),
                                  nullptr, g_psDownsample.GetAddressOf());

        // Sampler with bilinear filtering (clamped edges)
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, g_sampler.GetAddressOf());

        // Constant buffer
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(BlurCB);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&cbd, nullptr, g_cbBlur.GetAddressOf());

        initialized = true;
        logger::info("[BlurEffect] Initialized ({}x{} blur targets)", g_blurWidth, g_blurHeight);
    }

    void Shutdown() {
        g_vsFullscreen.Reset();
        g_psDownsample.Reset();
        g_psCopy.Reset();
        g_sampler.Reset();
        g_cbBlur.Reset();
        for (int i = 0; i < 2; i++) {
            g_blurTex[i].Reset();
            g_blurRTV[i].Reset();
            g_blurSRV[i].Reset();
        }
        g_backBufferCopy.Reset();
        g_backBufferSRV.Reset();
        g_device.Reset();
        initialized = false;
    }

    bool IsInitialized() { return initialized; }

    static void DrawFullscreenTriangle(ID3D11DeviceContext* ctx) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        ctx->Draw(3, 0);
    }

    static void SetViewport(ID3D11DeviceContext* ctx, UINT w, UINT h) {
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    }

    static void UpdateCB(ID3D11DeviceContext* ctx, float texelX, float texelY) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(g_cbBlur.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb = static_cast<BlurCB*>(mapped.pData);
            cb->texelSizeX = texelX;
            cb->texelSizeY = texelY;
            ctx->Unmap(g_cbBlur.Get(), 0);
        }
    }

    void RenderBlurredBackground(ID3D11DeviceContext* ctx, IDXGISwapChain* swapChain) {
        if (!initialized) return;

        // Save current render state
        ComPtr<ID3D11RenderTargetView> oldRTV;
        ComPtr<ID3D11DepthStencilView> oldDSV;
        ctx->OMGetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf());
        D3D11_VIEWPORT oldVP;
        UINT numVP = 1;
        ctx->RSGetViewports(&numVP, &oldVP);

        // Copy back buffer to our readable texture
        ComPtr<ID3D11Texture2D> backBuffer;
        swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        ctx->CopyResource(g_backBufferCopy.Get(), backBuffer.Get());

        // Set common state
        ctx->VSSetShader(g_vsFullscreen.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
        ID3D11Buffer* cbs[] = { g_cbBlur.Get() };
        ctx->PSSetConstantBuffers(0, 1, cbs);

        // Pass 1: Downsample back buffer -> blur target 0
        ctx->PSSetShader(g_psCopy.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = g_backBufferSRV.Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->OMSetRenderTargets(1, g_blurRTV[0].GetAddressOf(), nullptr);
        SetViewport(ctx, g_blurWidth, g_blurHeight);
        DrawFullscreenTriangle(ctx);

        // Blur passes: ping-pong between targets
        ctx->PSSetShader(g_psDownsample.Get(), nullptr, 0);
        UpdateCB(ctx, 1.0f / g_blurWidth, 1.0f / g_blurHeight);

        for (int i = 0; i < kBlurPasses; i++) {
            int src = i % 2;
            int dst = 1 - src;

            // Unbind SRV from output before binding as input
            ID3D11ShaderResourceView* nullSRV = nullptr;
            ctx->PSSetShaderResources(0, 1, &nullSRV);

            ctx->OMSetRenderTargets(1, g_blurRTV[dst].GetAddressOf(), nullptr);
            srv = g_blurSRV[src].Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            DrawFullscreenTriangle(ctx);
        }

        // Final pass: upscale blurred result back to the back buffer
        int finalSrc = kBlurPasses % 2;
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        ctx->PSSetShader(g_psCopy.Get(), nullptr, 0);
        ctx->OMSetRenderTargets(1, oldRTV.GetAddressOf(), nullptr);
        SetViewport(ctx, g_backBufferWidth, g_backBufferHeight);
        srv = g_blurSRV[finalSrc].Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        DrawFullscreenTriangle(ctx);

        // Clean up shader resources
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        // Restore state
        ctx->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
        ctx->RSSetViewports(1, &oldVP);
    }
}
