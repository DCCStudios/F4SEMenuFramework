#include "GamepadInput.h"
#include "Config.h"
#include "WindowManager.h"
#include "HotkeyManager.h"

#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>

namespace GamepadInput {

    // =======================================================================
    // XInput suppression — IAT hooks across ALL modules and ALL XInput DLLs
    // =======================================================================
    //
    // Why IAT hooks and not an inline (code-patch) hook:
    //  * Fallout4.exe statically imports xinput1_3.dll — NOT xinput1_4.dll
    //    (that one is only in the process because *we* link against it). An
    //    inline hook on a single DLL therefore missed the game engine
    //    entirely; the engine kept seeing real input and menus like the
    //    Favorites wheel still opened while our UI was up.
    //  * Steam's overlay / Steam Input often inline-hooks XInputGetState in
    //    the DLL the game imports to translate non-XInput pads (DualShock/
    //    DualSense) into XInput data. IAT patching composes cleanly with
    //    that: our hook runs first (the import slot points at us), and we
    //    forward to the export address — which executes Steam's chain.
    //  * Inline hooking requires relocating the function's first instructions
    //    into a trampoline; blindly copying N bytes can split an instruction
    //    and corrupt the function. IAT patching touches no code bytes at all.
    //
    // Known limitation: a module that resolves XInputGetState via
    // GetProcAddress at runtime bypasses its IAT and won't be suppressed.
    // The game engine (the thing that matters for menu bleed-through) uses a
    // static import, which IS covered.
    // =======================================================================

    using XInputGetState_t = DWORD(WINAPI*)(DWORD dwUserIndex, XINPUT_STATE* pState);

    // Every XInput DLL variant that could be present in the process. Order
    // matters for polling: xinput1_3 first because that's the game's import —
    // if Steam Input is translating a PlayStation controller, the translated
    // data is visible through THAT DLL's (Steam-hooked) export.
    struct XInputDllSlot {
        const wchar_t* name;              // DLL file name
        HMODULE module = nullptr;         // handle if loaded
        XInputGetState_t real = nullptr;  // export address (may be Steam-hooked code — fine)
    };
    static XInputDllSlot s_xinputDlls[] = {
        { L"xinput1_3.dll" },   // game's own import (DirectX SDK era)
        { L"xinput1_4.dll" },   // Windows 8+ (our own import, other plugins)
        { L"xinput9_1_0.dll" }, // Windows Vista+ generic
        { L"xinputuap.dll" },   // UWP shim, present on some Win10/11 setups
    };
    static constexpr size_t XINPUT_DLL_COUNT = sizeof(s_xinputDlls) / sizeof(s_xinputDlls[0]);

    static std::atomic<bool> s_suppressing{ false };

    // Shared suppression logic used by all per-DLL hook functions. Forwards to
    // the DLL's real export and zeroes the returned data while a framework
    // window is open, preserving the real connected/disconnected result code.
    static DWORD WINAPI HookedCommon(size_t dllIndex, DWORD dwUserIndex, XINPUT_STATE* pState) {
        XInputGetState_t real = s_xinputDlls[dllIndex].real;
        if (!real) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }

        DWORD result = real(dwUserIndex, pState);

        if (WindowManager::IsAnyWindowOpen()) {
            s_suppressing.store(true, std::memory_order_relaxed);
            // Keep the connection status but hide all buttons/sticks/triggers
            // from the caller (game engine, other plugins).
            if (result == ERROR_SUCCESS && pState) {
                DWORD packet = pState->dwPacketNumber;
                std::memset(pState, 0, sizeof(XINPUT_STATE));
                pState->dwPacketNumber = packet;
            }
        } else {
            s_suppressing.store(false, std::memory_order_relaxed);
        }
        return result;
    }

    // One concrete hook function per DLL variant (an IAT slot can only point
    // at a plain function, so the DLL index must be baked in statically).
    static DWORD WINAPI Hooked0(DWORD i, XINPUT_STATE* s) { return HookedCommon(0, i, s); }
    static DWORD WINAPI Hooked1(DWORD i, XINPUT_STATE* s) { return HookedCommon(1, i, s); }
    static DWORD WINAPI Hooked2(DWORD i, XINPUT_STATE* s) { return HookedCommon(2, i, s); }
    static DWORD WINAPI Hooked3(DWORD i, XINPUT_STATE* s) { return HookedCommon(3, i, s); }
    static XInputGetState_t const s_hookFns[XINPUT_DLL_COUNT] = { &Hooked0, &Hooked1, &Hooked2, &Hooked3 };

    // Returns the index into s_xinputDlls matching a DLL name from an import
    // descriptor, or SIZE_MAX if it's not an XInput DLL we handle.
    static size_t MatchXInputDll(const char* importName) {
        for (size_t idx = 0; idx < XINPUT_DLL_COUNT; ++idx) {
            // Import names are ANSI; compare case-insensitively against the
            // wide name by converting on the fly (ASCII-only names).
            const wchar_t* w = s_xinputDlls[idx].name;
            const char* a = importName;
            bool match = true;
            while (*w && *a) {
                if (towlower(*w) != static_cast<wint_t>(tolower(static_cast<unsigned char>(*a)))) {
                    match = false;
                    break;
                }
                ++w; ++a;
            }
            if (match && *w == 0 && *a == 0) {
                return idx;
            }
        }
        return SIZE_MAX;
    }

    // Patches XInputGetState import entries in a single module's IAT.
    // Returns the number of entries patched.
    static int PatchModuleIAT(HMODULE module, const wchar_t* moduleName) {
        auto* base = reinterpret_cast<uint8_t*>(module);

        // Validate PE headers defensively — we're walking every module in the
        // process and some (packed/hooked) modules can have odd layouts.
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0 || importDir.Size == 0) return 0;

        int patched = 0;
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
        for (; desc->Name != 0; ++desc) {
            const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
            size_t dllIdx = MatchXInputDll(dllName);
            if (dllIdx == SIZE_MAX) continue;
            if (!s_xinputDlls[dllIdx].real) continue;  // DLL not loaded / export missing

            // ILT (names/ordinals) and IAT (resolved addresses) run parallel.
            // Some linkers omit the ILT (OriginalFirstThunk == 0); fall back to
            // matching the IAT entry against the real export address.
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
            auto* ilt = desc->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk)
                : nullptr;

            for (size_t n = 0; iat[n].u1.Function != 0; ++n) {
                bool isTarget = false;
                if (ilt && ilt[n].u1.AddressOfData != 0 &&
                    !(ilt[n].u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                    auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + ilt[n].u1.AddressOfData);
                    isTarget = (std::strcmp(byName->Name, "XInputGetState") == 0);
                } else {
                    // Ordinal import or missing ILT — match by resolved address
                    isTarget = (iat[n].u1.Function ==
                                reinterpret_cast<ULONGLONG>(s_xinputDlls[dllIdx].real));
                }
                if (!isTarget) continue;

                // Already pointing at our hook (re-run of the install pass)
                if (iat[n].u1.Function == reinterpret_cast<ULONGLONG>(s_hookFns[dllIdx])) {
                    continue;
                }

                DWORD oldProtect;
                if (!VirtualProtect(&iat[n], sizeof(IMAGE_THUNK_DATA64), PAGE_READWRITE, &oldProtect)) {
                    continue;
                }
                iat[n].u1.Function = reinterpret_cast<ULONGLONG>(s_hookFns[dllIdx]);
                VirtualProtect(&iat[n], sizeof(IMAGE_THUNK_DATA64), oldProtect, &oldProtect);
                ++patched;

                logger::info("[GamepadInput] IAT hook: {} imports {} — XInputGetState patched",
                    std::filesystem::path(moduleName).filename().string(), dllName);
            }
        }
        return patched;
    }

    void InstallXInputHook() {
        // Resolve the real export address for every XInput DLL variant that's
        // loaded. GetProcAddress returns the export code address; if Steam has
        // inline-hooked it, calling it executes Steam's chain — exactly what
        // we want (translated controller data, no double-processing).
        for (auto& slot : s_xinputDlls) {
            if (!slot.module) {
                slot.module = GetModuleHandleW(slot.name);
                if (slot.module) {
                    slot.real = reinterpret_cast<XInputGetState_t>(
                        GetProcAddress(slot.module, "XInputGetState"));
                }
            }
        }

        bool anyLoaded = false;
        for (auto& slot : s_xinputDlls) {
            if (slot.real) {
                anyLoaded = true;
                logger::info("[GamepadInput] XInput DLL present: {}",
                    std::filesystem::path(slot.name).string());
            }
        }
        if (!anyLoaded) {
            logger::warn("[GamepadInput] No XInput DLL loaded — gamepad suppression unavailable");
            return;
        }

        // Never patch our own module: our Poll() must always read real state.
        HMODULE selfModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InstallXInputHook), &selfModule);

        // Walk every module currently loaded in the process and patch its IAT.
        // Idempotent — safe to call again later for late-loaded modules.
        int totalPatched = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Module32FirstW(snapshot, &entry)) {
                do {
                    if (entry.hModule == selfModule) continue;
                    // Don't patch the XInput DLLs themselves (forwarder stubs)
                    bool isXInputDll = false;
                    for (auto& slot : s_xinputDlls) {
                        if (slot.module == entry.hModule) { isXInputDll = true; break; }
                    }
                    if (isXInputDll) continue;

                    totalPatched += PatchModuleIAT(entry.hModule, entry.szModule);
                } while (Module32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }

        logger::info("[GamepadInput] XInput IAT hook pass complete — {} import entries patched", totalPatched);
    }

    bool IsSuppressing() {
        return s_suppressing.load(std::memory_order_relaxed);
    }

    // =======================================================================
    // Controller polling, toggle detection, hotkey dispatch
    // =======================================================================

    static bool s_controllerConnected = false;
    static unsigned short s_prevButtons = 0;
    static unsigned short s_currentButtons = 0;
    static unsigned char s_prevLeftTrigger = 0;
    static unsigned char s_prevRightTrigger = 0;
    static unsigned char s_currentLeftTrigger = 0;
    static unsigned char s_currentRightTrigger = 0;

    // Which (dll, user index) pair last had a connected controller — cached so
    // we don't scan 4 DLLs x 4 indices every frame. Rescan on disconnect or
    // every RESCAN_INTERVAL_MS while disconnected.
    static size_t s_activeDll = SIZE_MAX;
    static DWORD s_activeIndex = 0;
    static ULONGLONG s_lastRescanTick = 0;
    static constexpr ULONGLONG RESCAN_INTERVAL_MS = 1000;

    // Poll() can now be entered from two threads (game input thread and the
    // render-thread fallback). All the state above is single-writer via this
    // lock; a losing thread just skips the poll (fresh data is coming anyway).
    static std::mutex s_pollMutex;

    // Tick of the last completed poll — used by EnsurePolled() to decide when
    // the render thread needs to take over polling duty.
    static std::atomic<ULONGLONG> s_lastPollTick{ 0 };

    // --- Cross-thread snapshot for the ImGui navigation bridge ---
    // Poll() may run on the game's input thread; InjectImGuiEvents() runs on
    // the render thread (Present hook). Each field is a plain atomic so the
    // render thread always reads a coherent value without locking.
    static std::atomic<bool>           s_navConnected{ false };
    static std::atomic<unsigned short> s_navButtons{ 0 };
    static std::atomic<short>          s_navThumbLX{ 0 };
    static std::atomic<short>          s_navThumbLY{ 0 };
    static std::atomic<unsigned char>  s_navLeftTrigger{ 0 };
    static std::atomic<unsigned char>  s_navRightTrigger{ 0 };

    static constexpr unsigned char TRIGGER_THRESHOLD = 128;

    static std::chrono::steady_clock::time_point s_lastTogglePress{};
    static bool s_waitingForSecondPress = false;
    static constexpr int DOUBLE_PRESS_WINDOW_MS = 300;

    static std::chrono::steady_clock::time_point s_holdStartTime{};
    static bool s_holdActive = false;
    static constexpr int HOLD_THRESHOLD_MS = 400;

    WORD ConfigCodeToXInputMask(unsigned int configCode) {
        switch (configCode) {
            case 1:     return XINPUT_GAMEPAD_DPAD_UP;
            case 2:     return XINPUT_GAMEPAD_DPAD_DOWN;
            case 4:     return XINPUT_GAMEPAD_DPAD_LEFT;
            case 8:     return XINPUT_GAMEPAD_DPAD_RIGHT;
            case 16:    return XINPUT_GAMEPAD_START;
            case 32:    return XINPUT_GAMEPAD_BACK;
            case 64:    return XINPUT_GAMEPAD_LEFT_THUMB;
            case 128:   return XINPUT_GAMEPAD_RIGHT_THUMB;
            case 256:   return XINPUT_GAMEPAD_LEFT_SHOULDER;
            case 512:   return XINPUT_GAMEPAD_RIGHT_SHOULDER;
            case 4096:  return XINPUT_GAMEPAD_A;
            case 8192:  return XINPUT_GAMEPAD_B;
            case 16384: return XINPUT_GAMEPAD_X;
            case 32768: return XINPUT_GAMEPAD_Y;
            case 9:     return 0; // LT (analog, handled separately)
            case 10:    return 0; // RT (analog, handled separately)
            default:    return 0;
        }
    }

    static bool IsToggleButtonDown(unsigned int configCode) {
        if (configCode == 9) {
            return s_currentLeftTrigger >= TRIGGER_THRESHOLD;
        }
        if (configCode == 10) {
            return s_currentRightTrigger >= TRIGGER_THRESHOLD;
        }
        WORD mask = ConfigCodeToXInputMask(configCode);
        return mask != 0 && (s_currentButtons & mask) != 0;
    }

    static bool IsToggleButtonJustPressed(unsigned int configCode) {
        if (configCode == 9) {
            return (s_currentLeftTrigger >= TRIGGER_THRESHOLD) && (s_prevLeftTrigger < TRIGGER_THRESHOLD);
        }
        if (configCode == 10) {
            return (s_currentRightTrigger >= TRIGGER_THRESHOLD) && (s_prevRightTrigger < TRIGGER_THRESHOLD);
        }
        WORD mask = ConfigCodeToXInputMask(configCode);
        if (mask == 0) return false;
        return (s_currentButtons & mask) != 0 && (s_prevButtons & mask) == 0;
    }

    bool IsControllerConnected() {
        return s_navConnected.load(std::memory_order_relaxed);
    }

    WORD GetCurrentButtons() {
        return s_currentButtons;
    }

    // Reads the controller by calling the real XInput exports directly (these
    // calls never pass through our IAT hooks — our own module is not patched).
    // Tries the cached (dll, index) first; on failure rescans every loaded
    // XInput DLL and user indices 0..3. xinput1_3 (the game's import, and the
    // one Steam Input hooks for PlayStation-pad translation) is scanned first.
    static bool ReadControllerState(XINPUT_STATE& out) {
        // Fast path: cached device still connected
        if (s_activeDll != SIZE_MAX && s_xinputDlls[s_activeDll].real) {
            if (s_xinputDlls[s_activeDll].real(s_activeIndex, &out) == ERROR_SUCCESS) {
                return true;
            }
            // Lost it — force an immediate rescan below
            s_activeDll = SIZE_MAX;
            s_lastRescanTick = 0;
        }

        // Throttled rescan while disconnected
        ULONGLONG now = GetTickCount64();
        if (now - s_lastRescanTick < RESCAN_INTERVAL_MS) {
            return false;
        }
        s_lastRescanTick = now;

        for (size_t dll = 0; dll < XINPUT_DLL_COUNT; ++dll) {
            if (!s_xinputDlls[dll].real) continue;
            for (DWORD idx = 0; idx < 4; ++idx) {
                XINPUT_STATE state{};
                if (s_xinputDlls[dll].real(idx, &state) == ERROR_SUCCESS) {
                    s_activeDll = dll;
                    s_activeIndex = idx;
                    out = state;
                    logger::info("[GamepadInput] Controller found via {} index {}",
                        std::filesystem::path(s_xinputDlls[dll].name).string(), idx);
                    return true;
                }
            }
        }
        return false;
    }

    void Poll() {
        // Single-writer guard: if another thread is mid-poll, skip — the data
        // it produces is just as fresh.
        std::unique_lock lock(s_pollMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        s_lastPollTick.store(GetTickCount64(), std::memory_order_relaxed);

        XINPUT_STATE state{};
        bool connected = ReadControllerState(state);

        if (connected != s_controllerConnected) {
            logger::info("[GamepadInput] Controller {}", connected ? "connected" : "disconnected");
        }
        s_controllerConnected = connected;

        if (!connected) {
            s_prevButtons = 0;
            s_currentButtons = 0;
            s_prevLeftTrigger = 0;
            s_prevRightTrigger = 0;
            s_currentLeftTrigger = 0;
            s_currentRightTrigger = 0;
            // Publish "disconnected" to the render-thread nav bridge
            s_navConnected.store(false, std::memory_order_relaxed);
            s_navButtons.store(0, std::memory_order_relaxed);
            s_navThumbLX.store(0, std::memory_order_relaxed);
            s_navThumbLY.store(0, std::memory_order_relaxed);
            s_navLeftTrigger.store(0, std::memory_order_relaxed);
            s_navRightTrigger.store(0, std::memory_order_relaxed);
            return;
        }

        // Store previous frame state before updating
        s_prevButtons = s_currentButtons;
        s_prevLeftTrigger = s_currentLeftTrigger;
        s_prevRightTrigger = s_currentRightTrigger;

        // Update current frame state
        s_currentButtons = state.Gamepad.wButtons;
        s_currentLeftTrigger = state.Gamepad.bLeftTrigger;
        s_currentRightTrigger = state.Gamepad.bRightTrigger;

        // Publish the fresh snapshot for the ImGui navigation bridge
        s_navConnected.store(true, std::memory_order_relaxed);
        s_navButtons.store(state.Gamepad.wButtons, std::memory_order_relaxed);
        s_navThumbLX.store(state.Gamepad.sThumbLX, std::memory_order_relaxed);
        s_navThumbLY.store(state.Gamepad.sThumbLY, std::memory_order_relaxed);
        s_navLeftTrigger.store(state.Gamepad.bLeftTrigger, std::memory_order_relaxed);
        s_navRightTrigger.store(state.Gamepad.bRightTrigger, std::memory_order_relaxed);

        // --- Menu Toggle Detection ---
        unsigned int toggleCode = Config::ToggleKeyGamePad;
        uint8_t toggleMode = Config::ToggleModeGamePad;

        // Mode 3 = OFF, or no button configured
        if (toggleMode == 3 || toggleCode == 0) {
            return;
        }

        bool menuOpen = WindowManager::MainInterface && WindowManager::MainInterface->IsOpen.load();
        bool justPressed = IsToggleButtonJustPressed(toggleCode);

        // If menu is open, any press of the toggle button closes it
        if (menuOpen && justPressed) {
            WindowManager::Close();
            logger::debug("[GamepadInput] Toggle button — closing menu");
            return;
        }

        // Menu is closed — handle toggle modes
        if (!menuOpen) {
            switch (toggleMode) {
                case 0: // SinglePress
                    if (justPressed) {
                        WindowManager::Open();
                        logger::debug("[GamepadInput] Toggle button — opening menu (single press)");
                    }
                    break;

                case 1: // Hold
                    if (justPressed) {
                        s_holdStartTime = std::chrono::steady_clock::now();
                        s_holdActive = true;
                    } else if (s_holdActive && IsToggleButtonDown(toggleCode)) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - s_holdStartTime).count();
                        if (elapsed >= HOLD_THRESHOLD_MS) {
                            WindowManager::Open();
                            s_holdActive = false;
                            logger::debug("[GamepadInput] Toggle button held — opening menu");
                        }
                    } else if (!IsToggleButtonDown(toggleCode)) {
                        s_holdActive = false;
                    }
                    break;

                case 2: // DoublePress
                    if (justPressed) {
                        auto now = std::chrono::steady_clock::now();
                        if (s_waitingForSecondPress) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - s_lastTogglePress).count();
                            if (elapsed <= DOUBLE_PRESS_WINDOW_MS) {
                                WindowManager::Open();
                                s_waitingForSecondPress = false;
                                logger::debug("[GamepadInput] Toggle button double-pressed — opening menu");
                            } else {
                                s_lastTogglePress = now;
                            }
                        } else {
                            s_lastTogglePress = now;
                            s_waitingForSecondPress = true;
                        }
                    } else if (s_waitingForSecondPress) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - s_lastTogglePress).count();
                        if (elapsed > DOUBLE_PRESS_WINDOW_MS) {
                            s_waitingForSecondPress = false;
                        }
                    }
                    break;
            }
        }

        // --- Gamepad Hotkey Dispatch ---
        // Dispatch registered gamepad hotkeys when no menu is open.
        if (!WindowManager::IsAnyWindowOpen()) {
            WORD risingEdge = (s_currentButtons ^ s_prevButtons) & s_currentButtons;
            if (risingEdge != 0) {
                HotkeyManager::DispatchGamepad(static_cast<unsigned short>(risingEdge));
            }

            bool ltJustPressed = (s_currentLeftTrigger >= TRIGGER_THRESHOLD) && (s_prevLeftTrigger < TRIGGER_THRESHOLD);
            bool rtJustPressed = (s_currentRightTrigger >= TRIGGER_THRESHOLD) && (s_prevRightTrigger < TRIGGER_THRESHOLD);
            if (ltJustPressed) {
                HotkeyManager::DispatchGamepad(0);
                HotkeyManager::DispatchGamepadTrigger(9);
            }
            if (rtJustPressed) {
                HotkeyManager::DispatchGamepadTrigger(10);
            }
        }
    }

    void EnsurePolled() {
        // If the game's input thread polled recently, nothing to do. 100ms is
        // generous — the input thread normally polls every frame.
        constexpr ULONGLONG STALE_MS = 100;
        ULONGLONG last = s_lastPollTick.load(std::memory_order_relaxed);
        if (GetTickCount64() - last < STALE_MS) {
            return;
        }
        Poll();
    }

    // =======================================================================
    // ImGui gamepad navigation bridge (render thread)
    // =======================================================================
    // ImGui's Win32 backend gamepad poll is compiled out
    // (IMGUI_IMPL_WIN32_DISABLE_GAMEPAD in CMakeLists); this function feeds
    // the REAL controller snapshot (published by Poll(), which reads the raw
    // exports and is never suppressed) into ImGui's event queue instead.
    // Must be called every frame between ImGui_ImplWin32_NewFrame() and
    // ImGui::NewFrame().

    void InjectImGuiEvents() {
        auto& io = ImGui::GetIO();

        const bool connected = s_navConnected.load(std::memory_order_relaxed);
        // Only drive ImGui nav while one of our windows is actually open —
        // otherwise gamepad presses in normal gameplay would queue ImGui events.
        const bool active = connected && WindowManager::IsAnyWindowOpen();

        // Render-thread-local previous state for edge detection (Poll() may run
        // at a different cadence than Present, so we track our own edges here).
        static unsigned short s_prevNavButtons = 0;
        static bool s_wasActive = false;

        if (!active) {
            // On deactivation release every gamepad key we may have pressed so
            // ImGui doesn't see stuck buttons the next time the menu opens.
            if (s_wasActive) {
                // ImGuiKey_GamepadStart..ImGuiKey_GamepadRStickDown is the full
                // contiguous gamepad key range in this ImGui version (1.90.8).
                for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
                    io.AddKeyEvent(static_cast<ImGuiKey>(key), false);
                }
                s_prevNavButtons = 0;
                s_wasActive = false;
            }
            io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
            return;
        }

        // Nav requires the backend to advertise gamepad support
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        s_wasActive = true;

        const unsigned short buttons = s_navButtons.load(std::memory_order_relaxed);

        // Digital buttons — send an event only on state change
        auto mapButton = [&](WORD mask, ImGuiKey key) {
            const bool down = (buttons & mask) != 0;
            const bool was = (s_prevNavButtons & mask) != 0;
            if (down != was) {
                io.AddKeyEvent(key, down);
            }
        };
        mapButton(XINPUT_GAMEPAD_DPAD_UP,        ImGuiKey_GamepadDpadUp);
        mapButton(XINPUT_GAMEPAD_DPAD_DOWN,      ImGuiKey_GamepadDpadDown);
        mapButton(XINPUT_GAMEPAD_DPAD_LEFT,      ImGuiKey_GamepadDpadLeft);
        mapButton(XINPUT_GAMEPAD_DPAD_RIGHT,     ImGuiKey_GamepadDpadRight);
        mapButton(XINPUT_GAMEPAD_A,              ImGuiKey_GamepadFaceDown);   // activate / edit
        mapButton(XINPUT_GAMEPAD_B,              ImGuiKey_GamepadFaceRight);  // cancel / back
        mapButton(XINPUT_GAMEPAD_X,              ImGuiKey_GamepadFaceLeft);   // reset / unbind
        mapButton(XINPUT_GAMEPAD_Y,              ImGuiKey_GamepadFaceUp);     // settings
        mapButton(XINPUT_GAMEPAD_LEFT_SHOULDER,  ImGuiKey_GamepadL1);         // pane left
        mapButton(XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1);         // pane right
        mapButton(XINPUT_GAMEPAD_LEFT_THUMB,     ImGuiKey_GamepadL3);
        mapButton(XINPUT_GAMEPAD_RIGHT_THUMB,    ImGuiKey_GamepadR3);
        mapButton(XINPUT_GAMEPAD_START,          ImGuiKey_GamepadStart);
        mapButton(XINPUT_GAMEPAD_BACK,           ImGuiKey_GamepadBack);
        s_prevNavButtons = buttons;

        // Triggers — analog (ImGui uses these for tweak-fast/slow modifiers)
        const float lt = s_navLeftTrigger.load(std::memory_order_relaxed) / 255.0f;
        const float rt = s_navRightTrigger.load(std::memory_order_relaxed) / 255.0f;
        io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, lt > 0.15f, lt);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, rt > 0.15f, rt);

        // Left stick — analog nav (moves selection / scrolls like the D-pad).
        // Normalize past the standard XInput deadzone to a 0..1 range per axis.
        auto stickAxis = [&](short raw, ImGuiKey negKey, ImGuiKey posKey) {
            constexpr float DEADZONE = static_cast<float>(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);  // 7849
            constexpr float RANGE = 32767.0f;
            float v = static_cast<float>(raw);
            float mag = (std::abs(v) <= DEADZONE) ? 0.0f
                        : (std::abs(v) - DEADZONE) / (RANGE - DEADZONE);
            if (mag > 1.0f) mag = 1.0f;
            const float neg = (v < 0.0f) ? mag : 0.0f;
            const float pos = (v > 0.0f) ? mag : 0.0f;
            io.AddKeyAnalogEvent(negKey, neg > 0.1f, neg);
            io.AddKeyAnalogEvent(posKey, pos > 0.1f, pos);
        };
        stickAxis(s_navThumbLX.load(std::memory_order_relaxed), ImGuiKey_GamepadLStickLeft,  ImGuiKey_GamepadLStickRight);
        // XInput Y axis is + up; ImGui LStickUp is the "up" key
        const short ly = s_navThumbLY.load(std::memory_order_relaxed);
        stickAxis(static_cast<short>(ly == -32768 ? 32767 : -ly), ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
    }

} // namespace GamepadInput
