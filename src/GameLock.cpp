#include "GameLock.h"
#include "Config.h"
#include "imgui.h"
#include "WindowManager.h"

#include <Windows.h>

GameLock::State GameLock::lastState = GameLock::State::None;

void GameLock::SetState(State currentState) {
    if (lastState == currentState) {
        return;
    }

    State previousState = lastState;
    lastState = currentState;

    auto* controlMap = RE::ControlMap::GetSingleton();

    if (currentState == State::Locked) {
        if (controlMap) {
            controlMap->ignoreKeyboardMouse = true;
        }
        if (previousState == State::Unlocked || previousState == State::None) {
            ::ShowCursor(TRUE);
        }
    } else if (currentState == State::Unlocked) {
        if (controlMap) {
            controlMap->ignoreKeyboardMouse = false;
        }
        if (previousState == State::Locked) {
            ::ShowCursor(FALSE);
        }
        auto& io = ImGui::GetIO();
        io.ClearInputKeys();
    } else if (currentState == State::Resume) {
        if (controlMap) {
            controlMap->ignoreKeyboardMouse = false;
        }
    }
}
