#include "GameLock.h"
#include "Config.h"
#include "imgui.h"
#include "WindowManager.h"

#include <Windows.h>

GameLock::State GameLock::lastState = GameLock::State::None;
bool GameLock::gamePausedByUs = false;

void GameLock::SetGamePaused(bool shouldPause) {
    if (shouldPause && !gamePausedByUs) {
        // Add our +1 to the shared engine pause counter.
        // Other plugins (e.g. OAR) may independently hold their own +1;
        // the engine only resumes when the counter reaches 0.
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->menuMode += 1;
            gamePausedByUs = true;
            logger::debug("[GameLock] Engine pause applied (menuMode now {})", ui->menuMode);
        }
    } else if (!shouldPause && gamePausedByUs) {
        // Remove our +1, but guard against underflow in case another
        // plugin already decremented past our contribution.
        if (auto* ui = RE::UI::GetSingleton()) {
            if (ui->menuMode > 0) {
                ui->menuMode -= 1;
            }
            gamePausedByUs = false;
            logger::debug("[GameLock] Engine pause removed (menuMode now {})", ui->menuMode);
        }
    }
}

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

        // Freeze the game world if the user has the setting enabled.
        SetGamePaused(Config::FreezeTimeOnMenu);

    } else if (currentState == State::Unlocked) {
        // Leaving Locked → Unlocked: always release our engine pause.
        SetGamePaused(false);

        if (controlMap) {
            controlMap->ignoreKeyboardMouse = false;
        }
        if (previousState == State::Locked) {
            ::ShowCursor(FALSE);
        }
        auto& io = ImGui::GetIO();
        io.ClearInputKeys();

    } else if (currentState == State::Resume) {
        // "Resume Game" path: menu stays open but input is unblocked.
        // Release our engine pause so the world keeps running.
        SetGamePaused(false);

        if (controlMap) {
            controlMap->ignoreKeyboardMouse = false;
        }
    }
}
