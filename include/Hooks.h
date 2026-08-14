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

        // Device/context/window captured in CreateDeviceHook::thunk (we hold
        // one reference on each for the lifetime of the process). These are
        // the authoritative D3D11 objects the game renders with. Frame
        // Generation (Nexus 98208) and Upscaling (99130) replace the game's
        // swap chain with a hand-rolled DX12 proxy object whose GetDevice can
        // return null, so ImGui init must never depend on asking the swap
        // chain for its device.
        static inline ID3D11Device* capturedDevice = nullptr;
        static inline ID3D11DeviceContext* capturedContext = nullptr;
        static inline HWND capturedWindow = nullptr;
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

    // Dispatches the game's input-event queue to plugin callbacks registered
    // through AddInputEvent (export RegisterInpoutEvent) — the hook that makes
    // InputEventHandler::Process actually run.
    //
    // Two receivers are hooked because no single one carries every device in
    // every state (verified in game): the BSInputEventReceiver embedded in
    // PlayerCamera (+0x38) has keyboard in all states but loses the mouse wheel
    // while the player is sighted; PlayerControls (receiver at its own base,
    // offset 0 — the slot ScrollWheelWeaponSelect hooks) has the mouse/gamepad
    // in all states but no keyboard. We dispatch keyboard from the camera
    // receiver and mouse+gamepad from the controls receiver, so every event is
    // delivered exactly once regardless of ADS state. Both are chain-friendly
    // vtable-slot-0 hooks (save previous, forward), composing with MagnaScope /
    // UneducatedShooter / ScrollWheelWeaponSelect in install order. No Address
    // Library ID, so it works on every runtime.
    struct InputQueueHook {
        // PlayerCamera+0x38 receiver — dispatches keyboard (and any non-pointer).
        static void __fastcall cameraThunk(RE::BSInputEventReceiver* receiver, RE::InputEvent* queueHead);
        static inline void (*cameraOriginal)(RE::BSInputEventReceiver*, RE::InputEvent*) = nullptr;
        // PlayerControls receiver — dispatches mouse + gamepad.
        static void __fastcall controlsThunk(RE::BSInputEventReceiver* receiver, RE::InputEvent* queueHead);
        static inline void (*controlsOriginal)(RE::BSInputEventReceiver*, RE::InputEvent*) = nullptr;
        static void install();
    };

}
