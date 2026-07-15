#include "Hooks.h"
#include "Renderer.h"
#include "FontManager.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "WindowManager.h"
#include "InputEventHandler.h"
#include "HudManager.h"
#include "GameLock.h"
#include "TextureLoader.h"
#include "Event.h"
#include "Config.h"
#include "Input.h"
#include "HotkeyManager.h"
#include "BlurEffect.h"
#include "GamepadInput.h"
#include "UI.h"
#include "MCM/MCMLiveSync.h"
#include "MCM/MCMWidgetRenderer.h"

#include <d3d11.h>
#include <dxgi.h>
#include <chrono>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND g_gameWindow = nullptr;

void Hooks::Install() {
    CreateDeviceHook::install();
    ClipCursorHook::install();
}

void Hooks::InstallInputHooks() {
    DevicePollHook::install();
}

// ---------------------------------------------------------------------------
// CreateDeviceHook — write_call<5> on the D3D11CreateDeviceAndSwapChain call site
// REL::ID(224250) + 0x419 is the CALL instruction inside the game's renderer init.
// The IAT approach (REL::ID 254484) doesn't work because the game caches the
// function pointer before our plugin loads. GunMover uses this same call site.
// ---------------------------------------------------------------------------

void Hooks::CreateDeviceHook::install() {
    REL::Relocation<std::uintptr_t> callSite{ REL::ID(224250), 0x419 };
    auto& trampoline = F4SE::GetTrampoline();
    originalFunc = reinterpret_cast<FnCreateDeviceAndSwapChain>(
        trampoline.write_call<5>(callSite.address(), &thunk));
    logger::info("D3D11CreateDeviceAndSwapChain call-site hook at {:X}, original={:X}",
        callSite.address(), reinterpret_cast<std::uintptr_t>(originalFunc));
}

HRESULT __stdcall Hooks::CreateDeviceHook::thunk(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
    HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    HRESULT result = originalFunc(
        pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
        ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (FAILED(result)) {
        logger::error("Original D3D11CreateDeviceAndSwapChain failed: {:X}", static_cast<std::uint32_t>(result));
        return result;
    }

    logger::info("Game's D3D11 device created — hooking real SwapChain::Present");

    auto* vtbl = reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(*ppSwapChain));
    PresentHook::originalPresent = reinterpret_cast<decltype(PresentHook::originalPresent)>(vtbl[8]);
    REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtbl[8]),
        reinterpret_cast<std::uintptr_t>(&PresentHook::thunk));

    logger::info("IDXGISwapChain::Present VTable hook installed on real swap chain");

    g_gameWindow = ::GetActiveWindow();
    ::GetWindowRect(g_gameWindow, &ClipCursorHook::savedWindowRect);

    TextureLoader::Init(*ppDevice, *ppImmediateContext);

    return result;
}

// ---------------------------------------------------------------------------
// ClipCursorHook — IAT hook on ClipCursor (REL::ID 641385)
// Both Shadow-Boost-FO4 and GunMover hook this to allow mouse freedom when
// ImGui menus are open. Without this, the game constantly clips the cursor
// to the game window, preventing interaction with ImGui widgets.
// ---------------------------------------------------------------------------

void Hooks::ClipCursorHook::install() {
    REL::Relocation<std::uintptr_t> iatEntry{ REL::ID(641385) };
    originalClipCursor = reinterpret_cast<FnClipCursor>(
        *reinterpret_cast<std::uintptr_t*>(iatEntry.address()));
    REL::safe_write(iatEntry.address(),
        reinterpret_cast<std::uintptr_t>(&thunk));
    logger::info("ClipCursor IAT hook installed at {:X}", iatEntry.address());
}

BOOL __stdcall Hooks::ClipCursorHook::thunk(const RECT* lpRect) {
    if (WindowManager::ShouldTheGameBePaused() && lpRect) {
        return originalClipCursor(&savedWindowRect);
    }
    return originalClipCursor(lpRect);
}

// ---------------------------------------------------------------------------
// PresentHook — called every frame via the real swap chain
// ---------------------------------------------------------------------------

HRESULT __stdcall Hooks::PresentHook::thunk(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static bool initDone = false;

    if (!initDone) {
        initDone = true;
        logger::debug("[PresentHook] First call — initializing ImGui");

        ID3D11Device* device = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
        if (!device) {
            logger::error("Failed to get ID3D11Device from swapchain");
            return originalPresent(swapChain, syncInterval, flags);
        }

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);
        g_gameWindow = desc.OutputWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.IniFilename = nullptr;
        io.MouseDrawCursor = true;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
            logger::error("ImGui initialization failed (Win32)");
            device->Release();
            context->Release();
            return originalPresent(swapChain, syncInterval, flags);
        }

        if (!ImGui_ImplDX11_Init(device, context)) {
            logger::error("ImGui initialization failed (DX11)");
            device->Release();
            context->Release();
            return originalPresent(swapChain, syncInterval, flags);
        }

        UI::Renderer::initialized.store(true);
        logger::info("ImGui initialized.");

        // Initialize the GPU blur effect for background dimming
        BlurEffect::Init(device, swapChain);

        WndProcHook::func = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(desc.OutputWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
        if (!WndProcHook::func) {
            logger::error("SetWindowLongPtrA failed!");
        }

        Config::LoadStyle();

        auto regular = FontManager::LoadFonts(io, Config::FontSizeMedium);
        io.FontDefault = regular.defaultFont;

        FontManager::fontSizes["Big"] = FontManager::LoadFonts(io, Config::FontSizeBig);
        FontManager::fontSizes["Small"] = FontManager::LoadFonts(io, Config::FontSizeSmall);
        FontManager::fontSizes["Default"] = regular;

        io.Fonts->Build();

        device->Release();
        context->Release();

        logger::debug("[PresentHook] Initialization complete");
    }

    if (UI::Renderer::initialized.load()) {
        // Handle deferred font atlas rebuilds on the render thread.
        if (FontManager::IsReloadPending()) {
            FontManager::PerformReload();
        }

        // Apply GPU blur to the back buffer when background blur is active.
        // This runs before ImGui so the blurred scene is behind all UI.
        if (GameLock::blurAppliedByUs && BlurEffect::IsInitialized()) {
            ID3D11Device* dev = nullptr;
            swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev));
            if (dev) {
                ID3D11DeviceContext* ctx = nullptr;
                dev->GetImmediateContext(&ctx);
                if (ctx) {
                    BlurEffect::RenderBlurredBackground(ctx, swapChain);
                    ctx->Release();
                }
                dev->Release();
            }
        }

        Event::DispatchEvent(Event::EventType::kBeforeRender);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // Fallback poll: keeps controller detection alive even if the game's
        // input thread isn't polling its gamepad BSInputDevice (our vtable
        // thunk would then never run). No-op when the input thread is active.
        GamepadInput::EnsurePolled();

        // Feed real controller state into ImGui's event queue. The Win32
        // backend's own gamepad poll is compiled out (it would read zeroed
        // state through our XInputGetState hook); this bridge uses the
        // hook-bypassing snapshot from GamepadInput::Poll instead.
        GamepadInput::InjectImGuiEvents();

        if (g_gameWindow) {
            RECT rect;
            GetClientRect(g_gameWindow, &rect);
            auto& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(rect.right - rect.left);
            io.DisplaySize.y = static_cast<float>(rect.bottom - rect.top);
        }

        // Record popup/edit state BEFORE NewFrame: ImGui's nav-cancel runs
        // inside NewFrame and consumes B to close popups / stop editing, so
        // the back-cascade below needs this pre-frame snapshot to know the
        // press was already spent.
        UI::PreNewFrameGamepadSnapshot();

        ImGui::NewFrame();
        HudManager::Render();

        if (WindowManager::IsAnyWindowOpen()) {
            auto& io = ImGui::GetIO();
            if (!WindowManager::ShouldTheGameBePaused()) {
                io.MouseDrawCursor = false;
                GameLock::SetState(GameLock::State::Resume);
            } else {
                GameLock::SetState(GameLock::State::Locked);
                io.MouseDrawCursor = true;

                // B button (GamepadFaceRight) walks the back cascade first
                // (cancel capture -> close popup / stop editing -> settings
                // window -> content pane -> mod list); only when nothing is
                // left to back out of does it close the menu. Start always
                // closes immediately, mirroring ESC.
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                    if (!UI::HandleGamepadBack()) {
                        WindowManager::Close();
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
                    WindowManager::Close();
                }
            }
            // Draw dark overlay behind menu windows when blur setting is active.
            GameLock::RenderBackgroundOverlay();
            UI::Renderer::RenderWindows();
        } else {
            auto& io = ImGui::GetIO();
            io.MouseDrawCursor = false;
            GameLock::SetState(GameLock::State::Unlocked);
        }

        // End-of-frame MCM bookkeeping: fires OnMCMMenuOpen/Close on page
        // transitions (including "menu just closed") and clears per-frame
        // help-text state. Runs every frame, not only while windows are open.
        MCMWidgetRenderer::OnFrameEnd();

        // Deliver queued hotkey rebinds into the running native MCM as soon
        // as the pause menu (where MCM's root.mcm object lives) is open.
        MCMLiveSync::OnFrame();

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        FontManager::CleanFont();

        Event::DispatchEvent(Event::EventType::kAfterRender);
    }

    return originalPresent(swapChain, syncInterval, flags);
}

// ---------------------------------------------------------------------------
// WndProcHook
// ---------------------------------------------------------------------------

LRESULT Hooks::WndProcHook::thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!UI::Renderer::initialized.load()) {
        return CallWindowProcA(func, hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_KILLFOCUS) {
        auto& io = ImGui::GetIO();
        io.ClearInputKeys();
    }

    // --- Toggle key detection via WM_KEYDOWN (matches Shadow-Boost / GunMover pattern) ---
    static DoublePressDetector dpDetectorKb;
    static std::chrono::steady_clock::time_point holdStartTime;
    static bool holdKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        UINT scanCode = (lParam >> 16) & 0xFF;
        bool isFirstPress = (lParam & 0x40000000) == 0;

        if (scanCode == Config::ToggleKey && Config::ToggleMode != 3) {
            bool menuOpen = WindowManager::MainInterface && WindowManager::MainInterface->IsOpen.load();
            if (menuOpen) {
                if (isFirstPress) {
                    WindowManager::Close();
                    logger::debug("[WndProc] Toggle key (scan {:X}) — closing menu", scanCode);
                }
            } else {
                if (Config::ToggleMode == 0 && isFirstPress) {
                    WindowManager::Open();
                    logger::debug("[WndProc] Toggle key (scan {:X}) — opening menu (single press)", scanCode);
                } else if (Config::ToggleMode == 1) {
                    if (isFirstPress) {
                        holdStartTime = std::chrono::steady_clock::now();
                        holdKeyDown = true;
                    } else if (holdKeyDown) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - holdStartTime).count();
                        if (elapsed >= 400) {
                            WindowManager::Open();
                            holdKeyDown = false;
                            logger::debug("[WndProc] Toggle key held — opening menu (hold mode)");
                        }
                    }
                } else if (Config::ToggleMode == 2 && isFirstPress) {
                    dpDetectorKb.press();
                    if (dpDetectorKb) {
                        WindowManager::Open();
                        logger::debug("[WndProc] Toggle key double-pressed — opening menu");
                    }
                }
            }
        }

        if (wParam == VK_ESCAPE && isFirstPress) {
            if (WindowManager::ShouldTheGameBePaused()) {
                WindowManager::Close();
                logger::debug("[WndProc] ESC — closing blocking windows");
            }
        }

        // Dispatch registered plugin hotkeys when no blocking menu is active.
        if (isFirstPress && !WindowManager::ShouldTheGameBePaused()) {
            HotkeyManager::Dispatch(scanCode);
        }
    }

    if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        UINT scanCode = (lParam >> 16) & 0xFF;
        if (scanCode == Config::ToggleKey) {
            holdKeyDown = false;
        }
    }

    // --- Forward to ImGui and block game input when menu is active ---
    if (WindowManager::ShouldTheGameBePaused()) {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        return true;
    }

    return CallWindowProcA(func, hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Helper functions for input management
// ---------------------------------------------------------------------------

void DisableImGuiInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
}

void EnableImGuiInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavNoCaptureKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
}

// ---------------------------------------------------------------------------
// DevicePollHook — hooks BSInputDevice::Poll via VTable on each device
// ---------------------------------------------------------------------------

void Hooks::DevicePollHook::install() {
    auto* mgr = RE::BSInputDeviceManager::GetSingleton();
    if (!mgr) {
        logger::error("BSInputDeviceManager::GetSingleton() returned null — input hooks skipped");
        return;
    }

    auto* kbd = mgr->devices[static_cast<std::int32_t>(RE::INPUT_DEVICE::kKeyboard)];
    if (kbd) {
        void** vTable = *reinterpret_cast<void***>(kbd);
        DWORD oldProtect;
        VirtualProtect(&vTable[1], sizeof(void*), PAGE_READWRITE, &oldProtect);
        originalKeyboardPoll = reinterpret_cast<decltype(originalKeyboardPoll)>(vTable[1]);
        vTable[1] = reinterpret_cast<void*>(&keyboardThunk);
        VirtualProtect(&vTable[1], sizeof(void*), oldProtect, &oldProtect);
        logger::info("Keyboard BSInputDevice::Poll VTable hook installed");
    }

    auto* mouse = mgr->devices[static_cast<std::int32_t>(RE::INPUT_DEVICE::kMouse)];
    if (mouse) {
        void** vTable = *reinterpret_cast<void***>(mouse);
        DWORD oldProtect;
        VirtualProtect(&vTable[1], sizeof(void*), PAGE_READWRITE, &oldProtect);
        originalMousePoll = reinterpret_cast<decltype(originalMousePoll)>(vTable[1]);
        vTable[1] = reinterpret_cast<void*>(&mouseThunk);
        VirtualProtect(&vTable[1], sizeof(void*), oldProtect, &oldProtect);
        logger::info("Mouse BSInputDevice::Poll VTable hook installed");
    }

    auto* gamepad = mgr->devices[static_cast<std::int32_t>(RE::INPUT_DEVICE::kGamepad)];
    if (gamepad) {
        void** vTable = *reinterpret_cast<void***>(gamepad);
        DWORD oldProtect;
        VirtualProtect(&vTable[1], sizeof(void*), PAGE_READWRITE, &oldProtect);
        originalGamepadPoll = reinterpret_cast<decltype(originalGamepadPoll)>(vTable[1]);
        vTable[1] = reinterpret_cast<void*>(&gamepadThunk);
        VirtualProtect(&vTable[1], sizeof(void*), oldProtect, &oldProtect);
        logger::info("Gamepad BSInputDevice::Poll VTable hook installed");
    } else {
        logger::warn("No gamepad device found — gamepad toggle will rely on XInput polling");
    }
}

void __fastcall Hooks::DevicePollHook::keyboardThunk(RE::BSInputDevice* device, float pollDelta) {
    originalKeyboardPoll(device, pollDelta);

    if (!UI::Renderer::initialized.load()) return;

    if (WindowManager::ShouldTheGameBePaused()) {
        EnableImGuiInput();
    } else {
        DisableImGuiInput();
    }
}

void __fastcall Hooks::DevicePollHook::mouseThunk(RE::BSInputDevice* device, float pollDelta) {
    originalMousePoll(device, pollDelta);

    if (!UI::Renderer::initialized.load()) return;

    if (WindowManager::ShouldTheGameBePaused()) {
        EnableImGuiInput();
    } else {
        DisableImGuiInput();
    }
}

void __fastcall Hooks::DevicePollHook::gamepadThunk(RE::BSInputDevice* device, float pollDelta) {
    originalGamepadPoll(device, pollDelta);

    if (!UI::Renderer::initialized.load()) return;

    // Process gamepad toggle detection and hotkey dispatch via XInput polling.
    GamepadInput::Poll();

    // Block gamepad input from reaching the game whenever ANY menu window is open,
    // regardless of whether "freeze time" is enabled. This prevents the favorites
    // menu, VATS, pipboy, etc. from activating while navigating our overlay.
    if (WindowManager::IsAnyWindowOpen()) {
        EnableImGuiInput();

        // Clear the device's input state so the game sees no buttons pressed.
        // This is the proper BSInputDevice virtual that zeros all tracked button data.
        if (device) {
            device->ClearInputState();
        }
    } else {
        DisableImGuiInput();
    }
}
