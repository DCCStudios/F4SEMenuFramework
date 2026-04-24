#include "WindowManager.h"
#include "Event.h"

Window::Window() {
	Interface = new WindowInterface();
    Render = nullptr;
}


bool WindowManager::IsAnyWindowOpen() {
    auto it = std::find_if(WindowManager::Windows.begin(), WindowManager::Windows.end(),
                           [](Window* x) { return x->Interface->IsOpen.load(); });
    return it != WindowManager::Windows.end();
}


bool WindowManager::ShouldTheGameBePaused() {
    auto it = std::find_if(WindowManager::Windows.begin(), WindowManager::Windows.end(),
                           [](Window* x) { return x->Interface->IsOpen.load() && x->Interface->BlockUserInput.load();  });
    return it != WindowManager::Windows.end();
}

void WindowManager::Close() {
    WindowManager::MainInterface->BlockUserInput = true;
    WindowManager::ConfigInterface->BlockUserInput = true;
    ConfigInterface->IsOpen = false;
    MainInterface->IsOpen = false;
    // Close any blocking plugin windows so the game doesn't stay stuck paused.
    // Non-blocking (overlay) windows are intentionally left open — they persist
    // across menu toggles by design.
    for (auto* window : Windows) {
        if (window->Interface != MainInterface &&
            window->Interface != ConfigInterface &&
            window->Interface->BlockUserInput.load()) {
            window->Interface->IsOpen = false;
        }
    }
    Event::DispatchEvent(Event::EventType::kCloseMenu);
}

void WindowManager::Open() {
    MainInterface->IsOpen = true;
    Event::DispatchEvent(Event::EventType::kOpenMenu);
}
