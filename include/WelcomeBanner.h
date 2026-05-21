#pragma once

#include "HudManager.h"

// Displays a brief overlay on game startup showing the bound toggle key.
// Draws via HudManager (always-on HUD path), fades in/out, then self-unregisters.
namespace WelcomeBanner {
    void Show();
}
