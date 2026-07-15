#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace Hooks {

    void Install();
    void InstallInputHooks();

    struct WndProcHook {
        static LRESULT thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
        static inline WNDPROC func = nullptr;
    };

    struct CreateDeviceHook {
        using FnCreateDeviceAndSwapChain = HRESULT(__stdcall*)(
            IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
            const D3D_FEATURE_LEVEL*, UINT, UINT,
            const DXGI_SWAP_CHAIN_DESC*,
            IDXGISwapChain**, ID3D11Device**,
            D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

        static HRESULT __stdcall thunk(
            IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
            HMODULE Software, UINT Flags,
            const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
            UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
            IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
            D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

        static inline FnCreateDeviceAndSwapChain originalFunc = nullptr;
        static void install();
    };

    struct PresentHook {
        static HRESULT __stdcall thunk(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
        static inline HRESULT(__stdcall* originalPresent)(IDXGISwapChain*, UINT, UINT) = nullptr;
    };

    struct ClipCursorHook {
        using FnClipCursor = BOOL(__stdcall*)(const RECT*);
        static BOOL __stdcall thunk(const RECT* lpRect);
        static inline FnClipCursor originalClipCursor = nullptr;
        static inline RECT savedWindowRect{};
        static void install();
    };

    struct DevicePollHook {
        static void __fastcall keyboardThunk(RE::BSInputDevice* device, float pollDelta);
        static void __fastcall mouseThunk(RE::BSInputDevice* device, float pollDelta);
        static void __fastcall gamepadThunk(RE::BSInputDevice* device, float pollDelta);
        static inline void (*originalKeyboardPoll)(RE::BSInputDevice*, float) = nullptr;
        static inline void (*originalMousePoll)(RE::BSInputDevice*, float) = nullptr;
        static inline void (*originalGamepadPoll)(RE::BSInputDevice*, float) = nullptr;
        static void install();
    };

}
