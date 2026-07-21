#include "MCM/MCMLiveSync.h"
#include "MCM/MCMConflictCheck.h"
#include "MCM/MCMKeybindStore.h"
#include "HotkeyManager.h"

#include <atomic>
#include <map>
#include <mutex>
#include <unordered_set>

namespace MCMLiveSync {

    namespace {

        // One queued rebind. vk == 0 means "unbind".
        struct PendingPush {
            unsigned int vk = 0;
            int modifiers = 0;
        };

        // Key = modName + '\x1F' + keybindId (unit separator can't appear in either part).
        std::map<std::string, PendingPush> s_pending;
        std::map<std::string, Status> s_status;
        std::mutex s_mutex;

        // True while a flush task is queued on the F4SE UI thread, so OnFrame
        // (render thread, every frame) never double-schedules.
        std::atomic<bool> s_flushScheduled{ false };

        // Set by RequestPull; consumed by the next UI-thread flush.
        std::atomic<bool> s_pullPending{ false };

        std::string MakeKey(const std::string& a_modName, const std::string& a_keybindId) {
            return a_modName + '\x1F' + a_keybindId;
        }

        // CommonLibF4's GFx::Value has no boolean accessor; read the ValueUnion
        // byte directly (same ABI-stable trick as MCMConflictCheck::ReadGfxBool).
        bool ReadGfxBool(const RE::Scaleform::GFx::Value& a_val) {
            static_assert(sizeof(RE::Scaleform::GFx::Value) == 0x20);
            return *(reinterpret_cast<const std::uint8_t*>(std::addressof(a_val)) + 0x10) != 0;
        }

        // Same layout trick for kInt values (no GetInt accessor either). MCM's
        // SetKeybindInfo stores "keycode" with SetInt, so the union holds an
        // int32 at offset 0x10.
        std::int32_t ReadGfxInt(const RE::Scaleform::GFx::Value& a_val) {
            static_assert(sizeof(RE::Scaleform::GFx::Value) == 0x20);
            return *reinterpret_cast<const std::int32_t*>(
                reinterpret_cast<const std::uint8_t*>(std::addressof(a_val)) + 0x10);
        }

        // Invokes one of MCM's keybind functions on its "root.mcm" code object
        // and returns the Boolean result. The object's functions run native
        // code inside mcm.dll (KeybindManager), so a `true` here means the
        // running MCM's live state was actually changed.
        bool InvokeMCMBool(RE::Scaleform::GFx::Value& a_mcmObj, const char* a_fn,
                           const RE::Scaleform::GFx::Value* a_args, std::size_t a_numArgs) {
            RE::Scaleform::GFx::Value ret;
            if (!a_mcmObj.Invoke(a_fn, std::addressof(ret), a_args, a_numArgs)) {
                return false;
            }
            return ret.IsBoolean() && ReadGfxBool(ret);
        }

        // Runs on the F4SE UI task thread while the pause menu is (expected to
        // be) open. Delivers every queued push through root.mcm.
        void FlushOnUIThreadImpl();

        // Exception containment wrapper: the UI task thread has no handler
        // above us, so an escaped C++ exception would be a log-less CTD.
        void FlushOnUIThread() {
            try {
                FlushOnUIThreadImpl();
            } catch (const std::exception& e) {
                logger::error("[MCMLiveSync] EXCEPTION during keybind sync flush: {}", e.what());
                s_flushScheduled.store(false);
            } catch (...) {
                logger::error("[MCMLiveSync] EXCEPTION (non-std) during keybind sync flush");
                s_flushScheduled.store(false);
            }
        }

        void FlushOnUIThreadImpl() {
            // Re-validate on the UI thread — the menu may have closed between
            // scheduling and execution.
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->GetMenuOpen("PauseMenu")) {
                s_flushScheduled.store(false);
                return;  // stays pending; retried next time the menu opens
            }
            auto menu = ui->GetMenu("PauseMenu");
            if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
                s_flushScheduled.store(false);
                return;
            }
            auto* root = menu->uiMovie->asMovieRoot.get();

            // MCM's native code object, injected by mcm.dll's own Scaleform
            // callback when the pause menu movie loaded.
            RE::Scaleform::GFx::Value mcmObj;
            if (!root->GetVariable(std::addressof(mcmObj), "root.mcm") || !mcmObj.IsObject()) {
                // mcm.dll is present but its object is missing — unexpected;
                // don't retry forever, the file write still applies at load.
                logger::warn("[MCMLiveSync] root.mcm not found on PauseMenu movie — live sync unavailable");
                s_pullPending.store(false);
                std::lock_guard lock(s_mutex);
                for (auto& [key, push] : s_pending) {
                    s_status[key] = Status::Failed;
                }
                s_pending.clear();
                s_flushScheduled.store(false);
                return;
            }

            // Snapshot the queue so the Scaleform calls run without our lock held.
            std::map<std::string, PendingPush> work;
            {
                std::lock_guard lock(s_mutex);
                work.swap(s_pending);
            }

            // ---- Reverse sync (pull) first, so pending pushes (the user's
            // newest rebinds made in OUR menu) applied below win over whatever
            // the native MCM currently has for those same keys. ----
            if (s_pullPending.exchange(false)) {
                std::unordered_set<std::string> pushedKeys;
                for (const auto& [key, push] : work) {
                    pushedKeys.insert(key);
                }

                std::size_t changed = 0;
                for (const auto& m : MCMKeybindStore::GetAllMappings()) {
                    if (pushedKeys.contains(MakeKey(m.modName, m.keybindId))) {
                        continue;
                    }

                    // GetKeybind(modName, keybindID) returns an all-zero info
                    // object when the keybind isn't currently registered in
                    // MCM (keycode 0 == unbound) — exactly what we mirror.
                    const RE::Scaleform::GFx::Value args[2] = { m.modName.c_str(), m.keybindId.c_str() };
                    RE::Scaleform::GFx::Value info;
                    if (!mcmObj.Invoke("GetKeybind", std::addressof(info), args, 2) || !info.IsObject()) {
                        continue;
                    }
                    RE::Scaleform::GFx::Value keycodeVal;
                    if (!info.GetMember("keycode", std::addressof(keycodeVal))) {
                        continue;
                    }
                    const auto vk = static_cast<unsigned int>(ReadGfxInt(keycodeVal));

                    // Translate to the framework's DIK convention and import
                    // only real differences. ImportBinding (unlike SetBinding)
                    // does NOT notify MCMKeybindStore, so no write-back echo.
                    const unsigned int dik = vk != 0 ? MCMKeybindStore::VKToDIK(vk) : 0;
                    if (vk != 0 && dik == 0) {
                        continue;  // untranslatable key (e.g. gamepad-range code)
                    }
                    if (HotkeyManager::GetBinding(m.hotkeyId.c_str()) != dik) {
                        HotkeyManager::ImportBinding(m.hotkeyId.c_str(), dik);
                        MCMKeybindStore::ImportLiveBinding(m.modName, m.keybindId, vk);
                        ++changed;
                    }
                }
                if (changed > 0) {
                    logger::info("[MCMLiveSync] Pulled {} keybind change(s) from the running MCM", changed);
                }
            }

            for (const auto& [key, push] : work) {
                const auto sep = key.find('\x1F');
                const std::string modName = key.substr(0, sep);
                const std::string keybindId = key.substr(sep + 1);

                bool ok = false;
                if (push.vk == 0) {
                    // Unbind — ClearKeybind(modName, keybindID). "false" also
                    // means "was not bound", which is the desired end state.
                    const RE::Scaleform::GFx::Value args[2] = { modName.c_str(), keybindId.c_str() };
                    InvokeMCMBool(mcmObj, "ClearKeybind", args, 2);
                    ok = true;
                } else {
                    // MCM's KeybindManager keycodes are Windows VK, passed as
                    // GFx ints (its handlers type-check for kType_Int).
                    const RE::Scaleform::GFx::Value args[4] = {
                        modName.c_str(), keybindId.c_str(),
                        static_cast<std::int32_t>(push.vk),
                        static_cast<std::int32_t>(push.modifiers),
                    };
                    // RemapKeybind moves an *existing* registration to the new
                    // key. If the keybind isn't currently registered (fresh
                    // bind), fall back to SetKeybind, which registers it from
                    // the mod's keybinds.json definitions.
                    ok = InvokeMCMBool(mcmObj, "RemapKeybind", args, 4) ||
                         InvokeMCMBool(mcmObj, "SetKeybind", args, 4);
                }

                logger::info("[MCMLiveSync] Push '{}'/'{}' vk={} -> {}",
                    modName, keybindId, push.vk, ok ? "synced" : "FAILED");

                std::lock_guard lock(s_mutex);
                s_status[key] = ok ? Status::Synced : Status::Failed;
            }

            s_flushScheduled.store(false);
        }

    }  // anonymous namespace

    void QueuePush(const std::string& a_modName, const std::string& a_keybindId,
                   unsigned int a_vkKeycode, int a_modifiers) {
        // Only meaningful when the real MCM is running.
        if (!MCMConflictCheck::IsNativeMCMPresent()) {
            return;
        }
        std::lock_guard lock(s_mutex);
        const auto key = MakeKey(a_modName, a_keybindId);
        s_pending[key] = PendingPush{ a_vkKeycode, a_modifiers };
        s_status[key] = Status::Pending;
    }

    void RequestPull() {
        // Only meaningful when the real MCM is running.
        if (!MCMConflictCheck::IsNativeMCMPresent()) {
            return;
        }
        s_pullPending.store(true);
    }

    void OnFrame() {
        // Fast path: nothing queued (the common case, checked without locking
        // the queue by piggybacking on the schedule flag first).
        if (s_flushScheduled.load(std::memory_order_relaxed)) {
            return;
        }
        if (!s_pullPending.load(std::memory_order_relaxed)) {
            std::lock_guard lock(s_mutex);
            if (s_pending.empty()) {
                return;
            }
        }

        // MCM's root.mcm object only exists on the pause menu movie, so wait
        // for the user to open it (a single ESC after closing our overlay).
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->GetMenuOpen("PauseMenu")) {
            return;
        }

        // Scaleform must be touched from the game's UI thread, not the D3D
        // present thread we're called on.
        if (const auto* tasks = F4SE::GetTaskInterface()) {
            s_flushScheduled.store(true);
            tasks->AddUITask([]() { FlushOnUIThread(); });
        }
    }

    Status GetStatus(const std::string& a_modName, const std::string& a_keybindId) {
        std::lock_guard lock(s_mutex);
        auto it = s_status.find(MakeKey(a_modName, a_keybindId));
        return it != s_status.end() ? it->second : Status::None;
    }

}  // namespace MCMLiveSync
