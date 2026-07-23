#include "HudManager.h"

void HudManager::Render() {
    // Iterate over a snapshot: a callback is allowed to register or unregister
    // HUD elements (including itself) while it runs — the hotkey conflict
    // dialog, for example, calls HudManager::Unregister on itself the moment
    // the user confirms/cancels. Mutating `callbacks` while range-iterating it
    // directly would invalidate the live iterator (a latent crash). Copying the
    // small map of function pointers first makes that reentrancy safe.
    auto snapshot = callbacks;
    for (auto& item : snapshot) {
        item.second();
    }
}

int64_t HudManager::Register(HudElementCallback callback) {
    auto result = auto_increment++;
    callbacks[result] = callback;
    return result;
}

void HudManager::Unregister(uint64_t id) {
    auto it = callbacks.find(id);
    if (it != callbacks.end()) {
        callbacks.erase(it);
    }
}
