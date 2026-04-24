#include "HudManager.h"

void HudManager::Render() {
    for (auto item : callbacks) {
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
