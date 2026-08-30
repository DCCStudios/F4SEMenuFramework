#include "WelcomeBanner.h"
#include "HudManager.h"
#include "Config.h"
#include "Application.h"
#include "GamepadInput.h"
#include "imgui.h"

#include <chrono>

namespace WelcomeBanner {

    // --- Timing constants ---
    static constexpr float kFadeInDuration  = 0.5f;   // seconds
    static constexpr float kHoldDuration    = 10.0f;  // seconds at full opacity (doubled from 5.0)
    static constexpr float kFadeOutDuration = 2.0f;   // seconds
    static constexpr float kTotalDuration   = kFadeInDuration + kHoldDuration + kFadeOutDuration;

    // --- State ---
    // Timing is measured from a WALL CLOCK (steady_clock), started only once the
    // main menu is actually on screen. The old approach accumulated ImGui
    // DeltaTime from kGameDataReady, so the multi-second main-menu load (and any
    // slow-frame delta spike, which lands in io.DeltaTime as one huge value)
    // drained most of the duration before the menu was visible: the banner
    // arrived already faded out with its text invisible, or was gone entirely.
    // This mirrors OpenAnimationReplacer's UIWelcomeBanner fix (wall clock + a
    // gate on the menu being ready).
    static std::chrono::steady_clock::time_point startTime{};
    static bool    started     = false;
    static bool    active      = false;
    static int64_t hudHandle   = -1;

    // HUD callback invoked every frame by HudManager::Render().
    static void __stdcall RenderBanner() {
        if (!active) return;

        // Don't start the countdown (or draw) until the player can actually see
        // the banner. At startup the main menu isn't up for several seconds
        // after kGameDataReady; counting during that load is what made the
        // banner flash past. Gate on the main menu, then measure wall time.
        if (!started) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->GetMenuOpen("MainMenu")) {
                return;  // still loading / pre-menu
            }
            started = true;
            startTime = std::chrono::steady_clock::now();
        }

        const float displayTimer = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - startTime).count();
        if (displayTimer >= kTotalDuration) {
            // Done — unregister ourselves so we stop running.
            active = false;
            if (hudHandle >= 0) {
                HudManager::Unregister(static_cast<uint64_t>(hudHandle));
                hudHandle = -1;
            }
            return;
        }

        // Compute alpha for fade in / fade out.
        float alpha = 1.0f;
        if (displayTimer < kFadeInDuration) {
            alpha = displayTimer / kFadeInDuration;
        } else if (displayTimer > kFadeInDuration + kHoldDuration) {
            float fadeElapsed = displayTimer - (kFadeInDuration + kHoldDuration);
            alpha = 1.0f - (fadeElapsed / kFadeOutDuration);
        }
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        // Build the display string with the current toggle key name.
        std::string keyName = GetKeyName(Config::ToggleKey, RE::INPUT_DEVICE::kKeyboard);
        std::string message = std::format("Press [{}] to open Mod Control Panel", keyName);

        // Companion line for controller users: show the gamepad binding AND
        // its activation mode (hold / double-press change how the button is
        // used, so the mode is part of the instruction). Skipped when no
        // controller is connected or the gamepad toggle is unbound/off.
        std::string padMessage;
        if (GamepadInput::IsControllerConnected() &&
            Config::ToggleKeyGamePad != 0 && Config::ToggleModeGamePad != 3) {
            std::string padName = GetKeyName(Config::ToggleKeyGamePad, RE::INPUT_DEVICE::kGamepad);
            const char* verb = "Press";
            if (Config::ToggleModeGamePad == 1) {
                verb = "Hold";
            } else if (Config::ToggleModeGamePad == 2) {
                verb = "Double-press";
            }
            padMessage = std::format("{} [{}] on your controller", verb, padName);
        }

        // Position: top-left corner with a small margin.
        auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + 20.0f, viewport->WorkPos.y + 20.0f),
            ImGuiCond_Always);

        ImGui::SetNextWindowBgAlpha(0.6f * alpha);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoMove;

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));

        if (ImGui::Begin("##F4SEMenuFrameworkBanner", nullptr, flags)) {
            ImGui::TextUnformatted("F4SE Menu Framework");
            ImGui::TextUnformatted(message.c_str());
            if (!padMessage.empty()) {
                ImGui::TextUnformatted(padMessage.c_str());
            }
        }
        ImGui::End();

        ImGui::PopStyleVar(2);
    }

    void Show() {
        if (active) return;  // already showing
        active = true;
        started = false;         // clock starts on the first frame the main menu is up
        startTime = {};
        hudHandle = HudManager::Register(RenderBanner);
    }

}
