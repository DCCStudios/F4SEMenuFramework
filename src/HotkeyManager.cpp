#include "HotkeyManager.h"
#include "Application.h"
#include "HudManager.h"
#include "GamepadInput.h"
#include "WindowManager.h"
#include "MCM/MCMKeybindStore.h"
#include "imgui.h"
#include <filesystem>
#include <utility>
#include <algorithm>
#include <cctype>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RE/C/ControlMap.h"
#include "RE/U/UserEvents.h"

namespace {

    // Game-control conflict scan. The engine's own key bindings live in
    // RE::ControlMap: controlMaps[context]->deviceMappings[device] is an array
    // of {eventID, inputKey}. The encodings match the framework's — keyboard
    // inputKey is a DIK scan code, gamepad inputKey is the XInput button mask
    // (and 9/10 for the triggers) — so a plugin/user binding can be compared
    // directly against them.
    //
    // Only kMainGameplay is scanned: that's the context a framework hotkey
    // actually fires in (WndProc dispatches only while no blocking menu is
    // open). Menu/workshop/VATS contexts have their own maps, but a hotkey
    // that clashes there clashes only while that mode owns input, which is
    // exactly when the framework is not dispatching — listing them would be
    // noise. Mouse buttons (framework codes 256-260) are skipped: ControlMap's
    // mouse device uses a different encoding, so comparing risks false hits.
    std::vector<std::string> GetGameControlConflicts(unsigned int code, HotkeyDevice device) {
        std::vector<std::string> out;
        if (code == 0) return out;

        auto* controlMap = RE::ControlMap::GetSingleton();
        if (!controlMap) return out;

        RE::INPUT_DEVICE reDevice;
        if (device == HotkeyDevice::Gamepad) {
            reDevice = RE::INPUT_DEVICE::kGamepad;
        } else {
            if (code >= 256) return out;  // framework mouse-button code — skip
            reDevice = RE::INPUT_DEVICE::kKeyboard;
        }

        constexpr auto ctx = RE::UserEvents::INPUT_CONTEXT_ID::kMainGameplay;
        auto* context = controlMap->controlMaps[std::to_underlying(ctx)];
        if (!context) return out;

        const auto& mappings = context->deviceMappings[std::to_underlying(reDevice)];
        for (const auto& m : mappings) {
            if (m.inputKey < 0) continue;
            if (static_cast<unsigned int>(m.inputKey) != code) continue;
            const char* name = m.eventID.c_str();
            if (name && name[0] != '\0') {
                out.emplace_back(std::string("[Game] ") + name);
            }
        }
        return out;
    }

    // --- Chord modifier support ---

    // Current physical Ctrl/Shift/Alt state as a HotkeyMod bitmask. Read at
    // dispatch time from the WndProc (keyboard) or gamepad poll thread;
    // GetAsyncKeyState is valid from any thread.
    uint8_t CurrentModifiers() {
        uint8_t m = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= HotkeyMod::Ctrl;
        if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= HotkeyMod::Shift;
        if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= HotkeyMod::Alt;
        return m;
    }

    // A modified entry fires only on an exact modifier match; a plain entry
    // (modifiers == 0) fires regardless of modifier state, so every binding
    // that predates chords behaves exactly as before.
    bool ModifiersMatch(uint8_t entryMods, uint8_t held) {
        return entryMods == 0 || entryMods == held;
    }

    // "CTRL+SHIFT+F1" <-> (code, mods). Modifier tokens are unambiguous: the
    // physical modifier keys serialize as LEFTCONTROL / LEFTSHIFT / LEFTALT
    // (never bare CTRL/SHIFT/ALT), and no key name contains '+', so a legacy
    // value like "F1" parses cleanly as mods=0.
    std::string SerializeBinding(unsigned int code, uint8_t mods, RE::INPUT_DEVICE device) {
        std::string s;
        if (mods & HotkeyMod::Ctrl)  s += "CTRL+";
        if (mods & HotkeyMod::Shift) s += "SHIFT+";
        if (mods & HotkeyMod::Alt)   s += "ALT+";
        s += GetKeyName(static_cast<int>(code), device);
        return s;
    }

    void ParseBinding(const std::string& value, RE::INPUT_DEVICE device,
                      unsigned int& outCode, uint8_t& outMods) {
        outMods = 0;
        outCode = 0;
        for (const auto& token : SplitString(value, '+')) {
            std::string upper = token;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            if (upper == "CTRL")       outMods |= HotkeyMod::Ctrl;
            else if (upper == "SHIFT") outMods |= HotkeyMod::Shift;
            else if (upper == "ALT")   outMods |= HotkeyMod::Alt;
            else {
                int resolved = GetKeyBinding(token, device);
                if (resolved != 0) outCode = static_cast<unsigned int>(resolved);
            }
        }
    }

    // Human-readable "Ctrl+Shift+F1" for logs / dialog key column.
    std::string DisplayBinding(unsigned int code, uint8_t mods, RE::INPUT_DEVICE device) {
        std::string s;
        if (mods & HotkeyMod::Ctrl)  s += "Ctrl+";
        if (mods & HotkeyMod::Shift) s += "Shift+";
        if (mods & HotkeyMod::Alt)   s += "Alt+";
        s += GetKeyName(static_cast<int>(code), device);
        return s;
    }

}

// --- Plugin hotkey persistence file ---
// Plugin hotkey bindings live in their own user-data INI, NOT in
// F4SEMenuFramework.ini: the main INI ships with the mod, so every mod
// update used to overwrite the player's rebinds. This file is created at
// runtime (never shipped) and therefore survives updates; under MO2 it
// lands in overwrite like other user data. It sits inside the framework's
// asset folder (F4SEMenuFramework/, next to Fonts/ and Themes/) so the
// shared Plugins root stays uncluttered. The framework's own toggle keys
// ([General] ToggleKey / ToggleKeyGamePad) intentionally stay in the main
// INI.
namespace {

    // Relative to Data/F4SE/Plugins/ (the Ini class prepends that).
    constexpr const char* kPluginHotkeysFile = "F4SEMenuFramework/PluginHotkeys.ini";
    constexpr const char* kHotkeysSection = "Hotkeys";

    // The asset folder ships with the mod, but SaveFile cannot create
    // directories, so guard the first write in case a user pruned it.
    void EnsureHotkeysDir() {
        std::error_code ec;
        std::filesystem::create_directories("Data/F4SE/Plugins/F4SEMenuFramework", ec);
    }

    // One-time migration of bindings saved by older versions into
    // [Hotkeys] of F4SEMenuFramework.ini. Existing entries in the new file
    // win (a stale main INI must not clobber newer rebinds); the legacy
    // section is then removed so future mod updates carry no bindings.
    void MigrateLegacyHotkeys() {
        static bool done = false;
        if (done) return;
        done = true;

        Ini legacy("F4SEMenuFramework.ini");
        const auto keys = legacy.GetKeys(kHotkeysSection);
        if (keys.empty()) return;

        Ini fresh(kPluginHotkeysFile, /*createIfMissing=*/true);
        fresh.SetSection(kHotkeysSection);
        legacy.SetSection(kHotkeysSection);

        int copied = 0;
        for (const auto& key : keys) {
            const char* existing = fresh.GetString(key.c_str(), "");
            if (existing && existing[0] != '\0') continue;
            fresh.SetString(key.c_str(), legacy.GetString(key.c_str(), ""));
            ++copied;
        }

        // The new file must be safely on disk BEFORE the legacy section is
        // deleted; if that write fails, keep the legacy bindings so the
        // migration retries next launch instead of erasing the user's binds.
        // (copied == 0 means every key already exists in the new file, so
        // the legacy copy is redundant and safe to remove without a write.)
        bool savedOk = true;
        if (copied > 0) {
            EnsureHotkeysDir();
            savedOk = fresh.Save();
        }
        if (!savedOk) {
            logger::warn("[HotkeyManager] Could not write {} - keeping legacy "
                         "[Hotkeys] in F4SEMenuFramework.ini and retrying next launch",
                         kPluginHotkeysFile);
            return;
        }

        if (legacy.DeleteSection(kHotkeysSection)) {
            legacy.Save();
        }
        logger::info("[HotkeyManager] Migrated {} legacy binding(s) from "
                     "F4SEMenuFramework.ini to {}",
                     copied, kPluginHotkeysFile);
    }

}

// --- Conflict confirmation dialog state ---
// Centered modal that blocks hotkey rebinding until the user confirms or cancels.
namespace HotkeyConflictDialog {
    static bool active = false;
    static int64_t hudHandle = -1;
    static std::string pendingId;                    // the hotkey being rebound
    static unsigned int pendingScanCode = 0;         // the new key it wants
    static uint8_t pendingModifiers = 0;             // the modifiers it wants
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
            auto& entry = HotkeyManager::entriesByHandle[it->second];
            entry.scanCode = pendingScanCode;
            entry.modifiers = pendingModifiers;
            HotkeyManager::Save();
            // Keep the MCM Keybinds.json in sync for MCM-managed hotkeys
            // (no-op for regular framework/plugin hotkeys). MCM binds are
            // plain keys, so only the scan code is mirrored.
            MCMKeybindStore::OnFrameworkBindingChanged(pendingId, pendingScanCode);
            logger::info("[HotkeyManager] Binding for '{}' confirmed -> {}",
                         pendingId, keyName);
        }
        Close();
    }

    // ImGui popup identifier. Using a real modal popup (rather than a plain
    // Begin window with NoNav, which is what this used to be) buys three things
    // the old dialog lacked: gamepad/keyboard navigation reaches the buttons,
    // the popup captures input and dims the menu behind it, and the gamepad
    // "back" button (B / Circle) closes it for free via ImGui's NavCancel —
    // which the global back-cascade in Hooks.cpp already accounts for through
    // its pre-NewFrame OpenPopupStack snapshot, so B won't also close the menu.
    static constexpr const char* kPopupId = "Hotkey Conflict##HotkeyConflictConfirm";

    static void __stdcall Render() {
        if (!active) return;

        // Tie the dialog's lifetime to the menu. It is only ever spawned by a
        // rebind performed inside the framework menu, so if that menu is gone
        // (user pressed ESC / Start / the toggle key), the dialog must go too
        // instead of stranding a modal on the HUD during gameplay. Returning
        // before submitting the popup also lets ImGui auto-close it.
        if (!WindowManager::IsAnyWindowOpen()) {
            logger::info("[HotkeyManager] Menu closed with conflict dialog open — dismissing '{}'", pendingId);
            Close();
            return;
        }

        // Open on the first frame after Show(). Guarded so we don't re-open a
        // popup the user is actively dismissing.
        if (!ImGui::IsPopupOpen(kPopupId)) {
            ImGui::OpenPopup(kPopupId);
        }

        auto* viewport = ImGui::GetMainViewport();
        ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                      viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f));  // auto-height

        // NoNav is deliberately NOT set here (it was the bug): the modal needs
        // nav so a controller/keyboard can reach Cancel/Confirm.
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.08f, 0.02f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.8f, 0.5f, 0.1f, 0.7f));

        const bool visible = ImGui::BeginPopupModal(kPopupId, nullptr, flags);
        if (visible) {
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
            ImGui::TextWrapped("Items marked [Game] are your Fallout 4 controls; other entries are mod "
                               "hotkeys. All will respond to the same key press. Continue?");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Buttons — centered
            float buttonWidth = 100.0f;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float totalWidth = buttonWidth * 2 + spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.1f, 1.0f));
            const bool cancelClicked = ImGui::Button("Cancel", ImVec2(buttonWidth, 0));
            // Give the controller/keyboard an initial selection on Cancel (the
            // safe default) the first time the modal is navigated.
            ImGui::SetItemDefaultFocus();
            ImGui::PopStyleColor(2);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.7f, 0.3f, 1.0f));
            const bool confirmClicked = ImGui::Button("Confirm", ImVec2(buttonWidth, 0));
            ImGui::PopStyleColor(2);

            if (cancelClicked) {
                logger::info("[HotkeyManager] User cancelled rebinding '{}' to {}", pendingId, keyName);
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                Close();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
                return;
            }
            if (confirmClicked) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ApplyBinding();  // sets the binding, then Close()
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
                return;
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // BeginPopupModal returned false while we still consider the dialog
        // active => it was dismissed by ImGui's NavCancel (gamepad B / Esc).
        // Treat that as Cancel so state and the HUD registration are cleaned up.
        if (!visible && active) {
            logger::info("[HotkeyManager] Conflict dialog dismissed (back/Esc) for '{}'", pendingId);
            Close();
        }
    }

    static void Show(const std::string& id, unsigned int scanCode, uint8_t modifiers, const std::vector<std::string>& conflicts, const std::string& keyNameStr) {
        pendingId = id;
        pendingScanCode = scanCode;
        pendingModifiers = modifiers;
        conflictingIds = conflicts;
        keyName = keyNameStr;

        if (!active) {
            active = true;
            hudHandle = HudManager::Register(Render);
        }
    }
}

int64_t HotkeyManager::Register(const char* id, unsigned int defaultScanCode, HotkeyCallback callback) {
    return RegisterWithModifiers(id, defaultScanCode, 0, callback);
}

int64_t HotkeyManager::RegisterWithModifiers(const char* id, unsigned int defaultScanCode, uint8_t defaultModifiers, HotkeyCallback callback) {
    if (!id || !callback) return -1;

    std::string strId(id);

    // Guard: an unknown code is almost always a Windows virtual-key passed
    // where a DIK scan code was meant (VK_F1 0x70 vs DIK::F1 0x3B). Warn, but
    // still register — we can't be certain, and a hard reject would break any
    // legitimately-valid code this table happens not to list.
    if (defaultScanCode != 0 && !DIK::IsKnownCode(defaultScanCode)) {
        logger::warn("[HotkeyManager] '{}' registered with unknown keyboard code 0x{:X}. "
                     "If that is a Windows virtual-key (VK_*), pass the DirectInput scan code "
                     "instead — e.g. F1 is 0x3B (DIK::F1), not VK_F1 (0x70).",
                     strId, defaultScanCode);
    }

    // If already registered under this id, update the callback and return existing handle.
    auto itId = idToHandle.find(strId);
    if (itId != idToHandle.end()) {
        auto& entry = entriesByHandle[itId->second];
        // Re-registering the same id is legitimate (e.g. a plugin reloading),
        // but a DIFFERENT default from a second call site signals two features
        // accidentally sharing an id — they would fight over one binding.
        if (entry.defaultScanCode != defaultScanCode || entry.defaultModifiers != defaultModifiers) {
            logger::warn("[HotkeyManager] id '{}' re-registered with a different default "
                         "(was {}, now {}). Hotkey ids must be globally unique; "
                         "use a distinct \"ModName.Action\" id per hotkey.",
                         strId,
                         DisplayBinding(entry.defaultScanCode, entry.defaultModifiers, RE::INPUT_DEVICE::kKeyboard),
                         DisplayBinding(defaultScanCode, defaultModifiers, RE::INPUT_DEVICE::kKeyboard));
        }
        entry.callback = callback;
        return itId->second;
    }

    int64_t handle = autoIncrement++;

    HotkeyEntry entry;
    entry.id = strId;
    entry.defaultScanCode = defaultScanCode;
    entry.scanCode = defaultScanCode;
    entry.defaultModifiers = defaultModifiers;
    entry.modifiers = defaultModifiers;
    entry.callback = callback;
    entry.handle = handle;
    entry.device = HotkeyDevice::Keyboard;

    entriesByHandle[handle] = entry;
    idToHandle[strId] = handle;

    // If a persisted binding exists in INI, override the default.
    // (Load() populates entriesByHandle on startup; late registrations
    //  need to check the INI lazily.) The stored value may carry modifiers
    // ("CTRL+F1"); ParseBinding handles a legacy bare "F1" as mods=0.
    MigrateLegacyHotkeys();
    const auto ini = new Ini(kPluginHotkeysFile);
    ini->SetSection(kHotkeysSection);
    const char* val = ini->GetString(strId.c_str(), "");
    if (val && val[0] != '\0') {
        unsigned int resolvedCode = 0;
        uint8_t resolvedMods = 0;
        ParseBinding(std::string(val), RE::INPUT_DEVICE::kKeyboard, resolvedCode, resolvedMods);
        if (resolvedCode != 0) {
            entriesByHandle[handle].scanCode = resolvedCode;
            entriesByHandle[handle].modifiers = resolvedMods;
        }
    }
    delete ini;

    logger::info("[HotkeyManager] Registered '{}' -> {} (handle {})",
                 strId,
                 DisplayBinding(entriesByHandle[handle].scanCode, entriesByHandle[handle].modifiers, RE::INPUT_DEVICE::kKeyboard),
                 handle);
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

uint8_t HotkeyManager::GetModifiers(const char* id) {
    if (!id) return 0;
    auto it = idToHandle.find(std::string(id));
    if (it == idToHandle.end()) return 0;
    return entriesByHandle[it->second].modifiers;
}

void HotkeyManager::SetBinding(const char* id, unsigned int scanCode) {
    SetBindingWithModifiers(id, scanCode, 0);
}

void HotkeyManager::SetBindingWithModifiers(const char* id, unsigned int scanCode, uint8_t modifiers) {
    if (!id) return;
    auto it = idToHandle.find(std::string(id));
    if (it == idToHandle.end()) return;

    const auto reDevice = entriesByHandle[it->second].device == HotkeyDevice::Gamepad
        ? RE::INPUT_DEVICE::kGamepad
        : RE::INPUT_DEVICE::kKeyboard;

    // Check for conflicts before applying the new binding. Unbinding
    // (scanCode 0) never conflicts — every unbound hotkey shares code 0.
    if (scanCode != 0) {
        auto conflicts = GetConflicts(scanCode, modifiers, id);
        if (!conflicts.empty()) {
            // Don't apply yet — show confirmation dialog. Binding is applied
            // only if the user clicks Confirm.
            ShowConflictWarning(std::string(id), conflicts, scanCode, modifiers);
            return;
        }
    }

    // No conflict — apply immediately.
    entriesByHandle[it->second].scanCode = scanCode;
    entriesByHandle[it->second].modifiers = modifiers;
    Save();
    // Mirror MCM-managed hotkeys into MCM's Keybinds.json (no-op otherwise).
    MCMKeybindStore::OnFrameworkBindingChanged(std::string(id), scanCode);
    logger::info("[HotkeyManager] Binding for '{}' changed to {}",
                 id, DisplayBinding(scanCode, modifiers, reDevice));
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
    return GetConflicts(scanCode, 0, excludeId);
}

std::vector<std::string> HotkeyManager::GetConflicts(unsigned int scanCode, uint8_t modifiers, const char* excludeId) {
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

    // Two framework hotkeys only clash if they share the key AND the modifier
    // set — Shift+X and plain X are distinct bindings that never both fire.
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device == device && entry.scanCode == scanCode &&
            entry.modifiers == modifiers && entry.id != exclude) {
            conflicts.push_back(entry.id);
        }
    }

    // Also flag clashes with the player's actual game controls (Jump, Activate,
    // ...). Game controls are unmodified keys, so a chord binding (modifiers
    // != 0) can never clash with them — only check when modifiers == 0.
    if (modifiers == 0) {
        auto gameConflicts = GetGameControlConflicts(scanCode, device);
        conflicts.insert(conflicts.end(), gameConflicts.begin(), gameConflicts.end());
    }

    return conflicts;
}

void HotkeyManager::ShowConflictWarning(const std::string& hotkeyId, const std::vector<std::string>& conflicts, unsigned int scanCode, uint8_t modifiers) {
    std::string keyNameStr = DisplayBinding(scanCode, modifiers, RE::INPUT_DEVICE::kKeyboard);
    logger::warn("[HotkeyManager] Conflict: '{}' -> {} clashes with {} other binding(s)",
                 hotkeyId, keyNameStr, conflicts.size());
    HotkeyConflictDialog::Show(hotkeyId, scanCode, modifiers, conflicts, keyNameStr);
}

void HotkeyManager::Dispatch(unsigned int scanCode) {
    const uint8_t held = CurrentModifiers();
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device == HotkeyDevice::Keyboard && entry.scanCode == scanCode &&
            entry.callback && ModifiersMatch(entry.modifiers, held)) {
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
    return RegisterGamepadWithModifiers(id, defaultConfigCode, 0, callback);
}

int64_t HotkeyManager::RegisterGamepadWithModifiers(const char* id, unsigned int defaultConfigCode, uint8_t defaultModifiers, HotkeyCallback callback) {
    if (!id || !callback) return -1;

    std::string strId(id);

    if (defaultConfigCode != 0 && !GamepadCode::IsKnownCode(defaultConfigCode)) {
        logger::warn("[HotkeyManager] gamepad hotkey '{}' registered with unknown config code {}. "
                     "Use the framework gamepad codes (GamepadCode::A = 4096, GamepadCode::DpadUp = 1, "
                     "LT/RT = 9/10), not a raw XINPUT_GAMEPAD_* mask.",
                     strId, defaultConfigCode);
    }

    // If already registered under this id, update the callback and return existing handle.
    auto itId = idToHandle.find(strId);
    if (itId != idToHandle.end()) {
        auto& entry = entriesByHandle[itId->second];
        if (entry.defaultScanCode != defaultConfigCode || entry.defaultModifiers != defaultModifiers) {
            logger::warn("[HotkeyManager] gamepad id '{}' re-registered with a different default "
                         "(was {}, now {}). Hotkey ids must be globally unique.",
                         strId,
                         DisplayBinding(entry.defaultScanCode, entry.defaultModifiers, RE::INPUT_DEVICE::kGamepad),
                         DisplayBinding(defaultConfigCode, defaultModifiers, RE::INPUT_DEVICE::kGamepad));
        }
        entry.callback = callback;
        return itId->second;
    }

    int64_t handle = autoIncrement++;

    HotkeyEntry entry;
    entry.id = strId;
    entry.defaultScanCode = defaultConfigCode;
    entry.scanCode = defaultConfigCode;
    entry.defaultModifiers = defaultModifiers;
    entry.modifiers = defaultModifiers;
    entry.callback = callback;
    entry.handle = handle;
    entry.device = HotkeyDevice::Gamepad;

    entriesByHandle[handle] = entry;
    idToHandle[strId] = handle;

    // Check INI for persisted gamepad binding (may carry keyboard modifiers,
    // e.g. "CTRL+A"; ParseBinding handles a legacy bare "A" as mods=0).
    MigrateLegacyHotkeys();
    const auto ini = new Ini(kPluginHotkeysFile);
    ini->SetSection(kHotkeysSection);
    const char* val = ini->GetString(strId.c_str(), "");
    if (val && val[0] != '\0') {
        unsigned int resolvedCode = 0;
        uint8_t resolvedMods = 0;
        ParseBinding(std::string(val), RE::INPUT_DEVICE::kGamepad, resolvedCode, resolvedMods);
        if (resolvedCode != 0) {
            entriesByHandle[handle].scanCode = resolvedCode;
            entriesByHandle[handle].modifiers = resolvedMods;
        }
    }
    delete ini;

    logger::info("[HotkeyManager] Registered gamepad hotkey '{}' -> {} (handle {})",
                 strId, DisplayBinding(entriesByHandle[handle].scanCode, entriesByHandle[handle].modifiers, RE::INPUT_DEVICE::kGamepad), handle);
    return handle;
}

void HotkeyManager::DispatchGamepad(unsigned short buttonMask) {
    // Match each registered gamepad hotkey against the XInput bitmask of newly-pressed buttons.
    const uint8_t held = CurrentModifiers();
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device != HotkeyDevice::Gamepad) continue;
        if (!entry.callback) continue;
        if (!ModifiersMatch(entry.modifiers, held)) continue;

        WORD entryMask = GamepadInput::ConfigCodeToXInputMask(entry.scanCode);
        if (entryMask == 0) continue; // triggers handled separately

        if ((buttonMask & entryMask) != 0) {
            entry.callback();
        }
    }
}

void HotkeyManager::DispatchGamepadTrigger(unsigned int configCode) {
    // Dispatch gamepad hotkeys bound to analog triggers (config codes 9=LT, 10=RT).
    const uint8_t held = CurrentModifiers();
    for (auto& [handle, entry] : entriesByHandle) {
        if (entry.device != HotkeyDevice::Gamepad) continue;
        if (!entry.callback) continue;
        if (!ModifiersMatch(entry.modifiers, held)) continue;
        if (entry.scanCode == configCode) {
            entry.callback();
        }
    }
}

void HotkeyManager::Load() {
    MigrateLegacyHotkeys();
    const auto ini = new Ini(kPluginHotkeysFile);
    if (!ini->IsOpened()) {
        delete ini;
        return;
    }
    ini->SetSection(kHotkeysSection);

    // Update any already-registered entries with persisted values.
    // Keyboard and gamepad use different name tables (F2 vs LB), so resolve
    // with the device the entry was registered under.
    for (auto& [handle, entry] : entriesByHandle) {
        const char* val = ini->GetString(entry.id.c_str(), "");
        if (val && val[0] != '\0') {
            const auto device = entry.device == HotkeyDevice::Gamepad
                ? RE::INPUT_DEVICE::kGamepad
                : RE::INPUT_DEVICE::kKeyboard;
            unsigned int resolvedCode = 0;
            uint8_t resolvedMods = 0;
            ParseBinding(std::string(val), device, resolvedCode, resolvedMods);
            if (resolvedCode != 0) {
                entry.scanCode = resolvedCode;
                entry.modifiers = resolvedMods;
            }
        }
    }
    delete ini;
    logger::info("[HotkeyManager] Loaded persisted bindings.");
}

void HotkeyManager::Save() {
    MigrateLegacyHotkeys();
    // createIfMissing: the file does not ship with the mod, so the first
    // rebind a player makes must be able to create it.
    EnsureHotkeysDir();
    const auto ini = new Ini(kPluginHotkeysFile, /*createIfMissing=*/true);
    ini->SetSection(kHotkeysSection);

    for (auto& [handle, entry] : entriesByHandle) {
        const auto device = entry.device == HotkeyDevice::Gamepad
            ? RE::INPUT_DEVICE::kGamepad
            : RE::INPUT_DEVICE::kKeyboard;
        // "CTRL+F1" when modified, plain "F1" otherwise — so an unmodified
        // binding round-trips to the exact legacy format older versions wrote.
        std::string serialized = SerializeBinding(entry.scanCode, entry.modifiers, device);
        ini->SetString(entry.id.c_str(), serialized.c_str());
    }

    ini->Save();
    delete ini;
}
