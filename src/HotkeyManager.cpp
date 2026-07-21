#include "HotkeyManager.h"
#include "Application.h"
#include "HudManager.h"
#include "GamepadInput.h"
#include "MCM/MCMKeybindStore.h"
#include "imgui.h"

// --- Conflict confirmation dialog state ---
// Centered modal that blocks hotkey rebinding until the user confirms or cancels.
namespace HotkeyConflictDialog {
    static bool active = false;
    static int64_t hudHandle = -1;
    static std::string pendingId;                    // the hotkey being rebound
    static unsigned int pendingScanCode = 0;         // the new key it wants
    static std::vector<std::string> conflictingIds;  // existing hotkeys on that key
    static std::string keyName;                      // human-readable key name

    static void Close() {
        active = false;
        if (hudHandle >= 0) {
            HudManager::Unregister(static_cast<uint64_t>(hudHandle));
            hudHandle = -1;
        }
        pendingId.clear();
        conflictingIds.clear();
    }

    static void ApplyBinding() {
        // Actually set the binding now that the user confirmed.
        auto it = HotkeyManager::idToHandle.find(pendingId);
        if (it != HotkeyManager::idToHandle.end()) {
            HotkeyManager::entriesByHandle[it->second].scanCode = pendingScanCode;
            HotkeyManager::Save();
            // Keep the MCM Keybinds.json in sync for MCM-managed hotkeys
            // (no-op for regular framework/plugin hotkeys).
            MCMKeybindStore::OnFrameworkBindingChanged(pendingId, pendingScanCode);
            logger::info("[HotkeyManager] Binding for '{}' confirmed -> {}",
                         pendingId, keyName);
        }
        Close();
    }

    static void __stdcall Render() {
        if (!active) return;

        auto* viewport = ImGui::GetMainViewport();
        ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                      viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f));  // auto-height
        ImGui::SetNextWindowBgAlpha(0.95f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.08f, 0.02f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.8f, 0.5f, 0.1f, 0.7f));

        if (ImGui::Begin("##HotkeyConflictConfirm", nullptr, flags)) {
            // Title
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::TextUnformatted("Hotkey Conflict");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();

            // Body message
            ImGui::TextWrapped("Binding \"%s\" to [%s] conflicts with:", pendingId.c_str(), keyName.c_str());
            ImGui::Spacing();

            for (const auto& c : conflictingIds) {
                ImGui::BulletText("%s", c.c_str());
            }

            ImGui::Spacing();
            ImGui::TextWrapped("All conflicting hotkeys will fire on the same key press. Continue?");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Buttons — right-aligned
            float buttonWidth = 100.0f;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float totalWidth = buttonWidth * 2 + spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.1f, 1.0f));
            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
                logger::info("[HotkeyManager] User cancelled rebinding '{}' to {}", pendingId, keyName);
                Close();
            }
            ImGui::PopStyleColor(2);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.7f, 0.3f, 1.0f));
            if (ImGui::Button("Confirm", ImVec2(buttonWidth, 0))) {
                ApplyBinding();
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    static void Show(const std::string& id, unsigned int scanCode, const std::vector<std::string>& conflicts, const std::string& keyNameStr) {
        pendingId = id;
        pendingScanCode = scanCode;
        conflictingIds = conflicts;
        keyName = keyNameStr;

        if (!active) {
            active = true;
            hudHandle = HudManager::Register(Render);
        }
    }
}

int64_t HotkeyManager::Register(const char* id, unsigned int defaultScanCode, HotkeyCallback callback) {
    if (!id || !callback) return -1;

    std::string strId(id);

    // If already registered under this id, update the callback and return existing handle.
    auto itId = idToHandle.find(strId);
    if (itId != idToHandle.end()) {
        auto& entry = entriesByHandle[itId->second];
        entry.callback = callback;
        return itId->second;
    }

    int64_t handle = autoIncrement++;

    HotkeyEntry entry;
    entry.id = strId;
    entry.defaultScanCode = defaultScanCode;
    entry.scanCode = defaultScanCode;
    entry.callback = callback;
    entry.handle = handle;
    entry.device = HotkeyDevice::Keyboard;

    entriesByHandle[handle] = entry;
    idToHandle[strId] = handle;

    // If a persisted binding exists in INI, override the default.
    // (Load() populates entriesByHandle on startup; late registrations
    //  need to check the INI lazily.)
    const auto ini = new Ini("F4SEMenuFramework.ini");
    ini->SetSection("Hotkeys");
    const char* val = ini->GetString(strId.c_str(), "");
    if (val && val[0] != '\0') {
        int resolved = GetKeyBinding(std::string(val));
        if (resolved != 0) {
            entriesByHandle[handle].scanCode = static_cast<unsigned int>(resolved);
        }
    }
    delete ini;

    logger::info("[HotkeyManager] Registered '{}' -> {} (handle {})",
                 strId, GetKeyName(entriesByHandle[handle].scanCode, RE::INPUT_DEVICE::kKeyboard), handle);
    return handle;
}

void HotkeyManager::Unregister(int64_t handle) {
    auto it = entriesByHandle.find(handle);
    if (it == entriesByHandle.end()) return;

    idToHandle.erase(it->second.id);
    entriesByHandle.erase(it);
}

unsigned int HotkeyManager::GetBinding(const char* id) {
    if (!id) return 0;
    auto it = idToHandle.find(std::string(id));
    if (it == idToHandle.end()) return 0;
    return entriesByHandle[it->second].scanCode;
}

void HotkeyManager::SetBinding(const char* id, unsigned int scanCode) {
    if (!id) return;
    auto it = idToHandle.find(std::string(id));
    if (it == idToHandle.end()) return;

    // Check for conflicts before applying the new binding. Unbinding
    // (scanCode 0) never conflicts — every unbound hotkey shares code 0.
    if (scanCode != 0) {
        auto conflicts = GetConflicts(scanCode, id);
        if (!conflicts.empty()) {
            // Don't apply yet — show confirmation dialog. Binding is applied
            // only if the user clicks Confirm.
            ShowConflictWarning(std::string(id), conflicts, scanCode);
            return;
        }
    }

    // No conflict — apply immediately.
    entriesByHandle[it->second].scanCode = scanCode;
    Save();
    // Mirror MCM-managed hotkeys into MCM's Keybinds.json (no-op otherwise).
    MCMKeybindStore::OnFrameworkBindingChanged(std::string(id), scanCode);
    logger::info("[HotkeyManager] Binding for '{}' changed to {}",
                 id, GetKeyName(scanCode, RE::INPUT_DEVICE::kKeyboard));
}

bool HotkeyManager::IsRegistered(const char* id) {
    if (!id) return false;
    return idToHandle.find(std::string(id)) != idToHandle.end();
}

void HotkeyManager::ImportBinding(const char* id, unsigned int scanCode) {
    if (!id) return;
    auto it = idToHandle.find(std::string(id));
    if (it == idToHandle.end()) return;
    entriesByHandle[it->second].scanCode = scanCode;
    logger::info("[HotkeyManager] Imported binding for '{}' -> {}",
                 id, GetKeyName(scanCode, RE::INPUT_DEVICE::kKeyboard));
}

std::vector<std::string> HotkeyManager::GetConflicts(unsigned int scanCode, const char* excludeId) {
    std::vector<std::string> conflicts;
    // Scan code 0 means "unbound" — unbound hotkeys never conflict.
    if (scanCode == 0) return conflicts;
    std::string exclude = excludeId ? std::string(excludeId) : "";

    // Numeric codes are only comparable within one device: keyboard/mouse
    // code 256 (left mouse button) is unrelated to gamepad config code 256
    // (LB). Compare against entries on the same device as the excluded id's
    // entry (keyboard when unknown).
    HotkeyDevice device = HotkeyDevice::Keyboard;
    if (!exclude.empty()) {
        auto it = idToHandle.find(exclude);
        if (it != idToHandle.end()) {
            device = entriesByHandle[it->second].device;
        }
    }

    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device == device && entry.scanCode == scanCode && entry.id != exclude) {
            conflicts.push_back(entry.id);
        }
    }
    return conflicts;
}

void HotkeyManager::ShowConflictWarning(const std::string& hotkeyId, const std::vector<std::string>& conflicts, unsigned int scanCode) {
    std::string keyNameStr = GetKeyName(scanCode, RE::INPUT_DEVICE::kKeyboard);
    logger::warn("[HotkeyManager] Conflict: '{}' -> {} clashes with {} other hotkey(s)",
                 hotkeyId, keyNameStr, conflicts.size());
    HotkeyConflictDialog::Show(hotkeyId, scanCode, conflicts, keyNameStr);
}

void HotkeyManager::Dispatch(unsigned int scanCode) {
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device == HotkeyDevice::Keyboard && entry.scanCode == scanCode && entry.callback) {
            entry.isDown = true;
            entry.callback();
        }
    }
}

void HotkeyManager::SetReleaseCallback(const char* id, HotkeyCallback callback) {
    if (!id) return;
    auto itId = idToHandle.find(std::string(id));
    if (itId != idToHandle.end()) {
        entriesByHandle[itId->second].releaseCallback = callback;
    }
}

void HotkeyManager::DispatchUp(unsigned int scanCode) {
    for (auto& [handle, entry] : entriesByHandle) {
        // Only fire key-up for entries whose down-press we actually dispatched;
        // this also makes rebinding mid-press harmless.
        if (entry.device == HotkeyDevice::Keyboard && entry.scanCode == scanCode && entry.isDown) {
            entry.isDown = false;
            if (entry.releaseCallback) {
                entry.releaseCallback();
            }
        }
    }
}

int64_t HotkeyManager::RegisterGamepad(const char* id, unsigned int defaultConfigCode, HotkeyCallback callback) {
    if (!id || !callback) return -1;

    std::string strId(id);

    // If already registered under this id, update the callback and return existing handle.
    auto itId = idToHandle.find(strId);
    if (itId != idToHandle.end()) {
        auto& entry = entriesByHandle[itId->second];
        entry.callback = callback;
        return itId->second;
    }

    int64_t handle = autoIncrement++;

    HotkeyEntry entry;
    entry.id = strId;
    entry.defaultScanCode = defaultConfigCode;
    entry.scanCode = defaultConfigCode;
    entry.callback = callback;
    entry.handle = handle;
    entry.device = HotkeyDevice::Gamepad;

    entriesByHandle[handle] = entry;
    idToHandle[strId] = handle;

    // Check INI for persisted gamepad binding
    const auto ini = new Ini("F4SEMenuFramework.ini");
    ini->SetSection("Hotkeys");
    const char* val = ini->GetString(strId.c_str(), "");
    if (val && val[0] != '\0') {
        int resolved = GetKeyBinding(std::string(val), RE::INPUT_DEVICE::kGamepad);
        if (resolved != 0) {
            entriesByHandle[handle].scanCode = static_cast<unsigned int>(resolved);
        }
    }
    delete ini;

    logger::info("[HotkeyManager] Registered gamepad hotkey '{}' -> config code {} (handle {})",
                 strId, entriesByHandle[handle].scanCode, handle);
    return handle;
}

void HotkeyManager::DispatchGamepad(unsigned short buttonMask) {
    // Match each registered gamepad hotkey against the XInput bitmask of newly-pressed buttons.
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device != HotkeyDevice::Gamepad) continue;
        if (!entry.callback) continue;

        WORD entryMask = GamepadInput::ConfigCodeToXInputMask(entry.scanCode);
        if (entryMask == 0) continue; // triggers handled separately

        if ((buttonMask & entryMask) != 0) {
            entry.callback();
        }
    }
}

void HotkeyManager::DispatchGamepadTrigger(unsigned int configCode) {
    // Dispatch gamepad hotkeys bound to analog triggers (config codes 9=LT, 10=RT).
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device != HotkeyDevice::Gamepad) continue;
        if (!entry.callback) continue;
        if (entry.scanCode == configCode) {
            entry.callback();
        }
    }
}

void HotkeyManager::Load() {
    const auto ini = new Ini("F4SEMenuFramework.ini");
    if (!ini->IsOpened()) {
        delete ini;
        return;
    }
    ini->SetSection("Hotkeys");

    // Update any already-registered entries with persisted values.
    // Keyboard and gamepad use different name tables (F2 vs LB), so resolve
    // with the device the entry was registered under.
    for (auto& [handle, entry] : entriesByHandle) {
        const char* val = ini->GetString(entry.id.c_str(), "");
        if (val && val[0] != '\0') {
            const auto device = entry.device == HotkeyDevice::Gamepad
                ? RE::INPUT_DEVICE::kGamepad
                : RE::INPUT_DEVICE::kKeyboard;
            int resolved = GetKeyBinding(std::string(val), device);
            if (resolved != 0) {
                entry.scanCode = static_cast<unsigned int>(resolved);
            }
        }
    }
    delete ini;
    logger::info("[HotkeyManager] Loaded persisted bindings.");
}

void HotkeyManager::Save() {
    const auto ini = new Ini("F4SEMenuFramework.ini");
    ini->SetSection("Hotkeys");

    for (auto& [handle, entry] : entriesByHandle) {
        const auto device = entry.device == HotkeyDevice::Gamepad
            ? RE::INPUT_DEVICE::kGamepad
            : RE::INPUT_DEVICE::kKeyboard;
        std::string keyName = GetKeyName(entry.scanCode, device);
        ini->SetString(entry.id.c_str(), keyName.c_str());
    }

    ini->Save();
    delete ini;
}
