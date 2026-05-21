#include "WelcomeBanner.h"
#include "HudManager.h"
#include "Config.h"
#include "Application.h"
#include "imgui.h"

namespace WelcomeBanner {

    // --- Timing constants ---
    static constexpr float kFadeInDuration  = 0.5f;   // seconds
    static constexpr float kHoldDuration    = 5.0f;   // seconds at full opacity
    static constexpr float kFadeOutDuration = 2.0f;   // seconds
    static constexpr float kTotalDuration   = kFadeInDuration + kHoldDuration + kFadeOutDuration;

    // --- State ---
    static float  displayTimer = 0.0f;
    static bool   active       = false;
    static int64_t hudHandle   = -1;

    // HUD callback invoked every frame by HudManager::Render().
    static void __stdcall RenderBanner() {
        if (!active) return;

        displayTimer += ImGui::GetIO().DeltaTime;
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
        }
        ImGui::End();

        ImGui::PopStyleVar(2);
    }

    void Show() {
        if (active) return;  // already showing
        active = true;
        displayTimer = 0.0f;
        hudHandle = HudManager::Register(RenderBanner);
    }

}
