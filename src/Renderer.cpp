#include "Renderer.h"
#include "WindowManager.h"
#include "AutoTranslate.h"
#include "Config.h"
#include "Input.h"
#include "imgui_impl_dx11.h"
#include "Application.h"
#include "imgui_impl_win32.h"

// NOTE: ProcessOpenClose (menu toggle from the game's InputEvent queue) was
// removed as dead code — it had no caller since the F4 port moved toggle
// handling to WndProcHook (keyboard) and GamepadInput::Poll (pad). The plugin
// input API that lived beside it is now dispatched by Hooks::InputQueueHook.

void UI::Renderer::RenderWindows() {
    for (const auto window : WindowManager::Windows) {
        if (window->Interface->IsOpen) {
            // Auto-translate text drawn by third-party plugin windows
            // (no-op for the framework's own windows).
            AutoTranslate::Scope autoLoc(reinterpret_cast<void*>(window->Render));
            window->Render();
        }
    }
}

void UI::Renderer::install() {}
