#include "Input.h"

// NOTE: this file once carried a full InputEvent-queue -> ImGui translation
// layer (scan-code tables, ParseKeyFromKeyboard/Gamepad, TranslateButtonEvent,
// UI::TranslateInputEvent). It was dead code from the Skyrim original: ImGui
// gets keyboard/mouse from the Win32 WndProc backend and gamepad from
// GamepadInput::InjectImGuiEvents, and nothing ever called into it. The plugin
// input API it sat next to is dispatched by Hooks::InputQueueHook now; the
// canonical scan-code table lives in DIK.h.

void DoublePressDetector::press(){
    auto now = std::chrono::steady_clock::now();
    if (last_pressed_index) {
        last_pressed_times.second = now;
    } else {
        last_pressed_times.first = now;
    }
    increment();
};

DoublePressDetector::operator bool() const {
    const auto [first, second] = last_pressed_times;
    const int diff = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(second - first).count());
    return std::abs(diff) < double_press_threshold;

}
void DoublePressDetector::reset(){ last_pressed_times = {Timestamp::min(), Timestamp::min()};};
void DoublePressDetector::increment(){ last_pressed_index = !last_pressed_index; };

bool IsSupportedDevice(RE::INPUT_DEVICE device) {
    return device == RE::INPUT_DEVICE::kKeyboard || device == RE::INPUT_DEVICE::kGamepad;
}
