#include "Renderer.h"
#include "WindowManager.h"
#include "Config.h"
#include "Input.h"
#include "imgui_impl_dx11.h"
#include "Application.h"
#include "imgui_impl_win32.h"

bool UI::Renderer::ProcessOpenClose(RE::InputEvent* const* evns) {
    if (!*evns) return false;

    for (RE::InputEvent* e = *evns; e; e = e->next) {
        if (*e->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
        const auto* a_event = e->As<RE::ButtonEvent>();
        if (!a_event) continue;
        const auto temp_device = *a_event->device;
        if (!IsSupportedDevice(temp_device)) continue;
        const auto temp_toggleKey = temp_device == RE::INPUT_DEVICE::kKeyboard ? Config::ToggleKey : Config::ToggleKeyGamePad;
        if (static_cast<unsigned int>(a_event->idCode) == temp_toggleKey) {

            if (WindowManager::MainInterface->IsOpen.load() && a_event->QJustPressed()) {
                WindowManager::Close();
            } else {

                if (temp_device == RE::INPUT_DEVICE::kKeyboard) {
                    if (a_event->QJustPressed()) DoublePressDetectorKeyboard.press();

                    if (Config::ToggleMode == 0 && a_event->QJustPressed() ||
                        Config::ToggleMode == 1 && a_event->heldDownSecs > 0.4f ||
                        Config::ToggleMode == 2 && DoublePressDetectorKeyboard && a_event->QJustPressed()) {
                        WindowManager::Open();
                        return true;
                    };
                } else {
                    if (a_event->QJustPressed()) DoublePressDetectorGamepad.press();
                    if (Config::ToggleModeGamePad == 0 && a_event->QJustPressed() ||
                        Config::ToggleModeGamePad == 1 && a_event->heldDownSecs > 0.4f ||
                        Config::ToggleModeGamePad == 2 && DoublePressDetectorGamepad && a_event->QJustPressed()) {
                        WindowManager::Open();
                        return true;
                    };
                }

            }
        }
        if (a_event->idCode == 0x01 && temp_device == RE::INPUT_DEVICE::kKeyboard) {
            bool hasChanged = WindowManager::MainInterface->IsOpen.load();
            WindowManager::Close();
            return hasChanged;
        }
    }
    return false;
}

void UI::Renderer::RenderWindows() {
    for (const auto window : WindowManager::Windows) {
        if (window->Interface->IsOpen) {
            window->Render();
        }
    }
}

void UI::Renderer::install() {}
