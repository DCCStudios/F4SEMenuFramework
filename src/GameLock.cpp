#include "GameLock.h"
#include "Config.h"
#include "imgui.h"
#include "WindowManager.h"

#include <Windows.h>

GameLock::State GameLock::lastState = GameLock::State::None;
bool GameLock::gamePausedByUs = false;
bool GameLock::blurAppliedByUs = false;

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

void GameLock::SetBackgroundBlur(bool shouldBlur) {
    // Only use our ImGui dark overlay — freezeFrameMenuBG freezes the rendered
    // frame which makes the game look paused even when time is still running.
    if (shouldBlur && !blurAppliedByUs) {
        blurAppliedByUs = true;
        logger::debug("[GameLock] Background dim overlay enabled");
    } else if (!shouldBlur && blurAppliedByUs) {
        blurAppliedByUs = false;
        logger::debug("[GameLock] Background dim overlay disabled");
    }
}

void GameLock::RenderBackgroundOverlay() {
    if (!blurAppliedByUs) return;

    // Draw a dark semi-transparent overlay on top of the engine's blurred frame
    // to neutralize the blue tint and produce a clean darkened-blur look.
    auto* viewport = ImGui::GetMainViewport();
    auto* drawList = ImGui::GetBackgroundDrawList();
    drawList->AddRectFilled(
        viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 140)
    );
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
        // Apply background blur if the user has the setting enabled.
        SetBackgroundBlur(Config::BlurBackgroundOnMenu);

    } else if (currentState == State::Unlocked) {
        // Leaving Locked → Unlocked: always release our engine pause and blur.
        SetGamePaused(false);
        SetBackgroundBlur(false);

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
        // Release our engine pause and blur so the world keeps running.
        SetGamePaused(false);
        SetBackgroundBlur(false);

        if (controlMap) {
            controlMap->ignoreKeyboardMouse = false;
        }
    }
}
