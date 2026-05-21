#pragma once


namespace GameLock {
    enum State { None, Locked, Unlocked, Resume };
    extern State lastState;

    // Tracks whether *this plugin* currently holds a +1 on RE::UI::menuMode.
    // Other plugins (e.g. OAR) may independently hold their own +1 on the
    // same counter — the engine only resumes when the counter reaches 0.
    extern bool gamePausedByUs;

    // Tracks whether the dark background overlay should be rendered.
    extern bool blurAppliedByUs;

    void SetState(State currentState);

    // Increment / decrement RE::UI::menuMode by exactly 1 when transitioning
    // between paused and unpaused.  Safe to call redundantly — tracks
    // gamePausedByUs so it never double-increments or under-decrements.
    void SetGamePaused(bool shouldPause);

    // Enable / disable the dark background overlay rendered via ImGui.
    void SetBackgroundBlur(bool shouldBlur);

    // Draw the dark semi-transparent fullscreen overlay behind the menu.
    // Called from the render loop between NewFrame and window rendering.
    void RenderBackgroundOverlay();
}
