#include "HotkeyManager.h"
#include "Application.h"
#include "HudManager.h"
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

    // Check for conflicts before applying the new binding.
    auto conflicts = GetConflicts(scanCode, id);
    if (!conflicts.empty()) {
        // Don't apply yet — show confirmation dialog. Binding is applied
        // only if the user clicks Confirm.
        ShowConflictWarning(std::string(id), conflicts, scanCode);
        return;
    }

    // No conflict — apply immediately.
    entriesByHandle[it->second].scanCode = scanCode;
    Save();
    logger::info("[HotkeyManager] Binding for '{}' changed to {}",
                 id, GetKeyName(scanCode, RE::INPUT_DEVICE::kKeyboard));
}

std::vector<std::string> HotkeyManager::GetConflicts(unsigned int scanCode, const char* excludeId) {
    std::vector<std::string> conflicts;
    std::string exclude = excludeId ? std::string(excludeId) : "";
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.scanCode == scanCode && entry.id != exclude) {
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
        if (entry.scanCode == scanCode && entry.callback) {
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
    for (auto& [handle, entry] : entriesByHandle) {
        const char* val = ini->GetString(entry.id.c_str(), "");
        if (val && val[0] != '\0') {
            int resolved = GetKeyBinding(std::string(val));
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
        std::string keyName = GetKeyName(entry.scanCode, RE::INPUT_DEVICE::kKeyboard);
        ini->SetString(entry.id.c_str(), keyName.c_str());
    }

    ini->Save();
    delete ini;
}
