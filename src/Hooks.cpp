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
#include <cstring>

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
// Version-independent import helpers
//
// The game module imports D3D11CreateDeviceAndSwapChain and ClipCursor and, at
// static-init time (before F4SE plugins load), copies some import pointers into
// its own globals — which is why a late IAT patch alone is not always enough.
// FindGameImportSlots locates every 8-byte cell in the game module that holds
// the resolved import address: the IAT slot itself plus any cached copies in
// writable data. Overwriting all of them redirects every call path without an
// Address Library ID, which keeps these hooks working on runtimes whose IDs
// were never published (NG 1.10.980/984) and on future patches.
// ---------------------------------------------------------------------------

// Locates the game module's IAT slot for `dllName!funcName` (classic import
// directory walk). Returns nullptr when the import isn't found.
static std::uintptr_t* FindGameIATSlot(const char* dllName, const char* funcName) {
    auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
    if (!base) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* modName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(modName, dllName) != 0) continue;

        auto* thunkName = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* thunkAddr = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; thunkName->u1.AddressOfData; ++thunkName, ++thunkAddr) {
            if (thunkName->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunkName->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), funcName) == 0) {
                return reinterpret_cast<std::uintptr_t*>(&thunkAddr->u1.Function);
            }
        }
    }
    return nullptr;
}

// Scans the game module's writable data sections for cached copies of a
// resolved import address (globals the game filled at static-init time) and
// replaces each with `replacement`. Returns the number of cells patched.
static int PatchCachedImportPointers(std::uintptr_t importAddr, std::uintptr_t replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
    if (!base || !importAddr) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);

    int patched = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        // Cached import pointers live in initialized, writable data (.data);
        // read-only sections hold the IAT (handled separately) and constants.
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (!(section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)) continue;

        auto* begin = reinterpret_cast<std::uintptr_t*>(base + section->VirtualAddress);
        auto* end = reinterpret_cast<std::uintptr_t*>(
            base + section->VirtualAddress + section->Misc.VirtualSize - sizeof(std::uintptr_t) + 1);
        for (auto* p = begin; p < end; ++p) {
            if (*p == importAddr) {
                REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(p), replacement);
                ++patched;
            }
        }
    }
    return patched;
}

// ---------------------------------------------------------------------------
// CreateDeviceHook — intercepts the game's D3D11CreateDeviceAndSwapChain call
// so we can grab the real swap chain / device the moment they are created.
//
// Per-runtime strategy:
//  - OG 1.10.163: write_call<5> on the CALL inside the renderer-init function,
//    Address Library ID 224250 + 0x419 (proven in this project since 3.0).
//  - AE 1.11.137+: same call site, ID 4492363 + 0x410 (GunMover's field-tested
//    multi-runtime pair; both IDs verified present in every 1.11.x bin).
//  - NG 1.10.980/984: those address libraries never received an ID for this
//    function (4492363 is 1.11-only), so fall back to patching the import:
//    the IAT slot plus every cached copy of the resolved pointer in .data.
// ---------------------------------------------------------------------------

void Hooks::CreateDeviceHook::install() {
    if (!REX::FModule::IsRuntimeNG()) {
        // OG / AE: known call-site IDs. NG slot is a placeholder that is never
        // resolved (REL aborts the process on a missing-ID lookup, so we must
        // not even ask).
        static const REL::ID kRendererInitFn{ 224250, 0, 4492363 };
        const std::ptrdiff_t callOffset = REX::FModule::IsRuntimeOG() ? 0x419 : 0x410;
        const std::uintptr_t callSite = kRendererInitFn.address() + callOffset;
        auto& trampoline = REL::GetTrampoline();
        originalFunc = reinterpret_cast<FnCreateDeviceAndSwapChain>(
            trampoline.write_call<5>(callSite, &thunk));
        logger::info("D3D11CreateDeviceAndSwapChain call-site hook at {:X}, original={:X}",
            callSite, reinterpret_cast<std::uintptr_t>(originalFunc));
        return;
    }

    // NG 1.10.980/984: import-pointer patching (no Address Library ID exists).
    HMODULE d3d11 = ::GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) d3d11 = ::LoadLibraryW(L"d3d11.dll");
    const auto realFn = d3d11
        ? reinterpret_cast<std::uintptr_t>(::GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))
        : 0;
    if (!realFn) {
        logger::error("d3d11.dll!D3D11CreateDeviceAndSwapChain not resolvable — overlay disabled");
        return;
    }
    originalFunc = reinterpret_cast<FnCreateDeviceAndSwapChain>(realFn);

    int patched = 0;
    if (auto* slot = FindGameIATSlot("d3d11.dll", "D3D11CreateDeviceAndSwapChain")) {
        REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(slot),
            reinterpret_cast<std::uintptr_t>(&thunk));
        ++patched;
    }
    patched += PatchCachedImportPointers(realFn, reinterpret_cast<std::uintptr_t>(&thunk));
    if (patched > 0) {
        logger::info("D3D11CreateDeviceAndSwapChain import hook installed on NG runtime ({} pointer(s) patched)", patched);
    } else {
        logger::error("No D3D11CreateDeviceAndSwapChain import pointers found on NG runtime — overlay disabled");
    }
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
    REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(&vtbl[8]),
        reinterpret_cast<std::uintptr_t>(&PresentHook::thunk));

    logger::info("IDXGISwapChain::Present VTable hook installed on real swap chain");

    g_gameWindow = ::GetActiveWindow();
    ::GetWindowRect(g_gameWindow, &ClipCursorHook::savedWindowRect);

    TextureLoader::Init(*ppDevice, *ppImmediateContext);

    return result;
}

// ---------------------------------------------------------------------------
// ClipCursorHook — IAT hook on user32!ClipCursor
// Both Shadow-Boost-FO4 and GunMover hook this to allow mouse freedom when
// ImGui menus are open. Without this, the game constantly clips the cursor
// to the game window, preventing interaction with ImGui widgets.
//
// The slot is now located by walking the game module's import table by name
// instead of Address Library ID 641385 / 4823626 — same address on OG (it IS
// what 641385 pointed at), but works on every runtime including NG 980/984,
// whose address libraries never received the ID.
// ---------------------------------------------------------------------------

void Hooks::ClipCursorHook::install() {
    auto* slot = FindGameIATSlot("user32.dll", "ClipCursor");
    if (!slot) {
        logger::error("user32.dll!ClipCursor import not found in game module — cursor-unclip hook skipped");
        return;
    }
    originalClipCursor = reinterpret_cast<FnClipCursor>(*slot);
    REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(slot),
        reinterpret_cast<std::uintptr_t>(&thunk));
    logger::info("ClipCursor IAT hook installed at {:X}", reinterpret_cast<std::uintptr_t>(slot));
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

        // Degradation-aware build: retries with reduced CJK coverage if the
        // atlas would exceed D3D11's texture size limit (was a CTD).
        FontManager::BuildAtlasSafe(io);

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
        // Key-up counterpart of the hotkey dispatch above — needed by MCM
        // SendEvent keybinds (OnControlUp carries the held duration). Always
        // delivered so a press started in gameplay can't leave a hotkey stuck
        // "down" if a menu opened before release; DispatchUp itself only fires
        // for entries whose down-press was actually dispatched.
        HotkeyManager::DispatchUp(scanCode);
    }

    // --- Mouse buttons as hotkeys ---
    // The real MCM lets users bind mouse buttons; those live in the keyboard
    // binding space above the DIK range using the F4SE/Papyrus keycode
    // convention: 256 = left, 257 = right, 258 = middle, 259/260 = X1/X2.
    // Same gating as keys: presses only dispatch during gameplay, releases
    // always (so a press can't leave a SendEvent bind stuck "down").
    {
        unsigned int mouseCode = 0;
        bool mouseDown = false;
        switch (uMsg) {
            case WM_LBUTTONDOWN: mouseCode = 256; mouseDown = true; break;
            case WM_LBUTTONUP:   mouseCode = 256; break;
            case WM_RBUTTONDOWN: mouseCode = 257; mouseDown = true; break;
            case WM_RBUTTONUP:   mouseCode = 257; break;
            case WM_MBUTTONDOWN: mouseCode = 258; mouseDown = true; break;
            case WM_MBUTTONUP:   mouseCode = 258; break;
            case WM_XBUTTONDOWN: mouseCode = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 259 : 260; mouseDown = true; break;
            case WM_XBUTTONUP:   mouseCode = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 259 : 260; break;
            default: break;
        }
        if (mouseCode != 0) {
            if (mouseDown) {
                if (!WindowManager::ShouldTheGameBePaused()) {
                    HotkeyManager::Dispatch(mouseCode);
                }
            } else {
                HotkeyManager::DispatchUp(mouseCode);
            }
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
