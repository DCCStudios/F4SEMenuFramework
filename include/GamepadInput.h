#pragma once

#include <cstdint>
#include <chrono>

// Handles gamepad-specific input: menu toggle detection via XInput,
// button state tracking, and hotkey dispatch for gamepad buttons.
//
// Also owns the gamepad suppression system: IAT hooks on XInputGetState are
// installed in every loaded module (for every XInput DLL variant) so that the
// game engine — and any other native mod that polls XInput — reads a zeroed
// controller state while a framework window is open.
namespace GamepadInput {

    // Called each frame from the gamepad DevicePollHook thunk (game input
    // thread). Safe to also call from the render thread; an internal try-lock
    // makes concurrent calls a no-op.
    void Poll();

    // Render-thread fallback: polls the controller if the game's input thread
    // hasn't done so recently. This keeps controller detection working even if
    // the game stops polling its gamepad BSInputDevice (e.g. device considered
    // inactive) — the hint bar and ImGui nav bridge depend on it.
    void EnsurePolled();

    // Installs IAT hooks for XInputGetState across all loaded modules and all
    // XInput DLL variants (xinput1_3 = the game's own import, xinput1_4,
    // xinput9_1_0, xinputuap). Idempotent — call again later (e.g. at
    // kGameDataReady) to patch modules that were loaded after plugin load.
    void InstallXInputHook();

    // Returns true if a controller is currently connected.
    bool IsControllerConnected();

    // Returns the current buttons bitmask (XINPUT_GAMEPAD format).
    unsigned short GetCurrentButtons();

    // Translates a Config gamepad button code to an XInput bitmask value.
    unsigned short ConfigCodeToXInputMask(unsigned int configCode);

    // Returns true if the XInput hook is currently suppressing input.
    bool IsSuppressing();

    // ------------------------------------------------------------------
    // ImGui gamepad navigation bridge
    // ------------------------------------------------------------------
    // The IAT hooks return ZEROED state to every caller while a framework
    // window is open — including any XInput poll ImGui might do (the Win32
    // backend gamepad poll is compiled out via IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    // anyway). We feed real controller state to ImGui ourselves: Poll()
    // snapshots buttons/sticks by calling the real XInput exports directly
    // (never through the hooked IAT slots), and InjectImGuiEvents() (render
    // thread, called between the backend NewFrame and ImGui::NewFrame)
    // converts the snapshot into io.AddKeyEvent / io.AddKeyAnalogEvent calls
    // so ImGui's built-in gamepad navigation works.

    // Called from the Present hook every frame, render thread only.
    void InjectImGuiEvents();

}
