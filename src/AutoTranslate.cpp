#include "AutoTranslate.h"

#include "PluginLocalization.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace AutoTranslate {

    namespace {

        // Per-plugin-module state. Lives for the session once created.
        struct PluginCtx {
            std::string name;                 // translation folder name
            bool sectionNamed = false;        // name came from AddSectionItem

            // Cached "does this plugin have any translations" so the per-
            // widget hot path can bail without touching PluginLocalization.
            // Revalidated whenever the localization generation changes.
            unsigned long long gen = 0;
            bool hasStrings = false;

            // Interned compositions (translated "Visible##id" labels and
            // zero-separated combo blobs). Keyed by the original full
            // string; bounded because only translation HITS compose.
            std::map<std::string, std::string> composed;

            // One-shot warn set for rejected format-string translations,
            // so a bad translation logs once instead of every frame.
            std::set<std::string> warnedFormats;

            // Capture mode: unique visible strings this plugin drew.
            std::set<std::string> captured;
            bool captureDirty = false;
        };

        constexpr size_t kMaxComposed = 4096;   // per plugin, paranoia cap
        constexpr size_t kMaxCaptured = 5000;   // per plugin
        constexpr size_t kMaxCaptureLen = 512;  // bytes per captured string

        std::mutex s_mutex;
        std::map<HMODULE, std::unique_ptr<PluginCtx>> s_ctxByModule;
        // Callback-address resolution cache. nullptr value = "framework's
        // own callback or unresolvable", cached so it is decided once.
        std::map<void*, PluginCtx*> s_ctxByCallback;

        std::atomic<bool> s_enabled{true};
        std::atomic<bool> s_capture{false};

        // Only the render thread runs plugin callbacks, so a thread_local
        // is the correct (and lock-free) way to carry "current plugin".
        thread_local PluginCtx* t_ctx = nullptr;

        HMODULE SelfModule() {
            static HMODULE self = [] {
                HMODULE m{};
                ::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&SelfModule), &m);
                return m;
            }();
            return self;
        }

        HMODULE ModuleFromAddress(void* address) {
            if (!address) return nullptr;
            HMODULE m{};
            if (!::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address), &m)) {
                return nullptr;
            }
            return m;
        }

        // DLL file name stem ("FPGunplayOverhaul" from "...\\FPGunplayOverhaul.dll"),
        // the fallback translation folder name for modules that never
        // registered a section.
        std::string ModuleStem(HMODULE mod) {
            wchar_t buf[MAX_PATH]{};
            if (!::GetModuleFileNameW(mod, buf, MAX_PATH)) return {};
            std::filesystem::path p(buf);
            // stem() can throw only on allocation failure; caller catches.
            const std::wstring stem = p.stem().wstring();
            // Narrow via UTF-8 (plugin DLL names are ASCII in practice).
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (len <= 1) return {};
            std::string out(static_cast<size_t>(len) - 1, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), -1, out.data(), len,
                                  nullptr, nullptr);
            return out;
        }

        // Resolves the plugin context for a callback address, creating and
        // caching it on first sight. Returns nullptr for the framework's
        // own callbacks (its UI and the MCM layer have their own pipeline).
        PluginCtx* ResolveCtx(void* callback) {
            if (!callback) return nullptr;
            {
                std::lock_guard lock(s_mutex);
                if (auto it = s_ctxByCallback.find(callback); it != s_ctxByCallback.end()) {
                    return it->second;
                }
            }
            PluginCtx* result = nullptr;
            const HMODULE mod = ModuleFromAddress(callback);
            if (mod && mod != SelfModule()) {
                std::lock_guard lock(s_mutex);
                auto& slot = s_ctxByModule[mod];
                if (!slot) {
                    slot = std::make_unique<PluginCtx>();
                    slot->name = ModuleStem(mod);
                }
                if (!slot->name.empty()) result = slot.get();
            }
            std::lock_guard lock(s_mutex);
            s_ctxByCallback[callback] = result;
            return result;
        }

        // Revalidates the ctx's hasStrings cache against the localization
        // generation. Render thread only.
        void RefreshCtx(PluginCtx* ctx) {
            const unsigned long long gen = PluginLocalization::Generation();
            if (ctx->gen == gen) return;
            const bool has = PluginLocalization::KeyCount(ctx->name) > 0;
            std::lock_guard lock(s_mutex);
            ctx->gen = gen;
            ctx->hasStrings = has;
            // Compositions were built from the previous table contents.
            ctx->composed.clear();
            ctx->warnedFormats.clear();
        }

        // Records a string for capture mode (visible part only, must
        // contain at least one letter so pure IDs/numbers/format tokens
        // like "%.2f" don't clutter the skeleton).
        void Capture(PluginCtx* ctx, const char* visible, size_t len) {
            if (!s_capture.load(std::memory_order_relaxed)) return;
            if (!visible || len == 0 || len > kMaxCaptureLen) return;
            bool hasLetter = false;
            for (size_t i = 0; i < len; ++i) {
                const unsigned char c = static_cast<unsigned char>(visible[i]);
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80) {
                    hasLetter = true;
                    break;
                }
            }
            if (!hasLetter) return;
            std::lock_guard lock(s_mutex);
            if (ctx->captured.size() >= kMaxCaptured) return;
            if (ctx->captured.emplace(visible, len).second) {
                ctx->captureDirty = true;
            }
        }

        // Interns a composed string in the ctx and returns a stable pointer,
        // or nullptr when the paranoia cap is hit.
        const char* Intern(PluginCtx* ctx, const std::string& key, std::string&& value) {
            std::lock_guard lock(s_mutex);
            if (auto it = ctx->composed.find(key); it != ctx->composed.end()) {
                return it->second.c_str();
            }
            if (ctx->composed.size() >= kMaxComposed) return nullptr;
            return ctx->composed.emplace(key, std::move(value)).first->second.c_str();
        }

        // Core label lookup against a specific ctx (shared by Label and
        // NodeTitle). Returns nullptr on miss.
        const char* LookupLabel(PluginCtx* ctx, const char* label) {
            // Whole-string hit first (covers labels without ID suffixes and
            // translators who included the suffix in their key).
            if (const char* hit = PluginLocalization::TryGet(ctx->name, label)) {
                return hit;
            }
            // "Visible##id": translate the visible part, keep the suffix.
            std::string suffix;
            const std::string visible = PluginLocalization::SplitVisibleLabel(label, &suffix);
            if (suffix.empty() || visible.empty()) return nullptr;
            const char* hit = PluginLocalization::TryGet(ctx->name, visible.c_str());
            if (!hit) return nullptr;
            return Intern(ctx, label, std::string(hit) + suffix);
        }

    }

    void SetEnabled(bool on) { s_enabled.store(on, std::memory_order_relaxed); }
    bool IsEnabled() { return s_enabled.load(std::memory_order_relaxed); }
    void SetCaptureEnabled(bool on) { s_capture.store(on, std::memory_order_relaxed); }
    bool IsCaptureEnabled() { return s_capture.load(std::memory_order_relaxed); }

    void NoteSectionPath(void* renderFn, const char* fullPath) {
        try {
            if (!renderFn || !fullPath || !*fullPath) return;
            const HMODULE mod = ModuleFromAddress(renderFn);
            if (!mod || mod == SelfModule()) return;

            // First path segment = the SetSection() name = the folder the
            // explicit Translate() API already uses. Preferring it keeps
            // one folder per mod for both mechanisms.
            const char* slash = std::strchr(fullPath, '/');
            const std::string section = slash ? std::string(fullPath, slash)
                                              : std::string(fullPath);
            if (section.empty()) return;

            std::lock_guard lock(s_mutex);
            auto& slot = s_ctxByModule[mod];
            if (!slot) {
                slot = std::make_unique<PluginCtx>();
                slot->name = ModuleStem(mod);
            }
            if (!slot->sectionNamed) {
                slot->name = section;
                slot->sectionNamed = true;
                slot->gen = 0; // force hasStrings revalidation for the new name
            }
        } catch (...) {
            // Registration bookkeeping must never break AddSectionItem.
        }
    }

    Scope::Scope(void* callbackAddress) : previous_(t_ctx) {
        t_ctx = nullptr;
        try {
            if (!s_enabled.load(std::memory_order_relaxed) &&
                !s_capture.load(std::memory_order_relaxed)) {
                return;
            }
            PluginCtx* ctx = ResolveCtx(callbackAddress);
            if (ctx) {
                RefreshCtx(ctx);
                t_ctx = ctx;
            }
        } catch (...) {
            t_ctx = nullptr;
        }
    }

    Scope::~Scope() { t_ctx = static_cast<PluginCtx*>(previous_); }

    const char* Label(const char* label) {
        PluginCtx* ctx = t_ctx;
        if (!label || !*label || !ctx) return label;
        try {
            std::string suffix;
            const std::string visible = PluginLocalization::SplitVisibleLabel(label, &suffix);
            Capture(ctx, visible.c_str(), visible.size());
            if (!s_enabled.load(std::memory_order_relaxed) || !ctx->hasStrings) return label;
            if (const char* hit = LookupLabel(ctx, label)) return hit;
        } catch (...) {
        }
        return label;
    }

    const char* Format(const char* fmt) {
        PluginCtx* ctx = t_ctx;
        if (!fmt || !*fmt || !ctx) return fmt;
        try {
            Capture(ctx, fmt, std::strlen(fmt));
            if (!s_enabled.load(std::memory_order_relaxed) || !ctx->hasStrings) return fmt;
            const char* hit = PluginLocalization::TryGet(ctx->name, fmt);
            if (!hit) return fmt;
            // The CTD guard: a translation may only replace a format string
            // when it consumes varargs identically. Reject and warn once.
            if (!PluginLocalization::FormatSpecsCompatible(fmt, hit)) {
                std::lock_guard lock(s_mutex);
                if (ctx->warnedFormats.emplace(fmt).second) {
                    logger::warn(
                        "AutoTranslate: rejected translation for '{}' in '{}': "
                        "format specifiers do not match the original (would crash)",
                        fmt, ctx->name);
                }
                return fmt;
            }
            return hit;
        } catch (...) {
        }
        return fmt;
    }

    const char* Range(const char* text, const char* textEnd) {
        // A (begin, end) substring view cannot be replaced: the caller's
        // end pointer would point into the wrong string.
        if (textEnd) return text;
        return Label(text);
    }

    const char* const* LabelArray(const char* const items[], int count) {
        PluginCtx* ctx = t_ctx;
        if (!items || count <= 0 || !ctx) return items;
        try {
            // Reused per call; ImGui consumes the array before returning,
            // and combos/listboxes cannot nest within one call stack.
            thread_local std::vector<const char*> buf;
            buf.assign(items, items + count);
            bool any = false;
            for (int i = 0; i < count; ++i) {
                const char* t = Label(items[i]);
                if (t != items[i]) {
                    buf[static_cast<size_t>(i)] = t;
                    any = true;
                }
            }
            if (any) return buf.data();
        } catch (...) {
        }
        return items;
    }

    const char* ZeroSeparated(const char* itemsBlob) {
        PluginCtx* ctx = t_ctx;
        if (!itemsBlob || !*itemsBlob || !ctx) return itemsBlob;
        try {
            // Walk "A\0B\0\0", translating each item.
            std::string translated;
            bool any = false;
            const char* p = itemsBlob;
            while (*p) {
                const size_t len = std::strlen(p);
                const char* t = Label(p);
                if (t != p) any = true;
                translated.append(t);
                translated.push_back('\0');
                p += len + 1;
            }
            if (!any) return itemsBlob;
            translated.push_back('\0'); // double terminator

            // Intern keyed by the original blob (embedded NULs included so
            // distinct blobs sharing a first item don't collide).
            const std::string key(itemsBlob, static_cast<size_t>(p - itemsBlob));
            if (const char* stable = Intern(ctx, key, std::move(translated))) {
                return stable;
            }
        } catch (...) {
        }
        return itemsBlob;
    }

    const char* NodeTitle(void* renderFn, const char* title) {
        if (!title || !*title) return title;
        try {
            if (!s_enabled.load(std::memory_order_relaxed) &&
                !s_capture.load(std::memory_order_relaxed)) {
                return title;
            }
            PluginCtx* ctx = ResolveCtx(renderFn);
            if (!ctx) return title;
            RefreshCtx(ctx);
            Capture(ctx, title, std::strlen(title));
            if (!s_enabled.load(std::memory_order_relaxed) || !ctx->hasStrings) return title;
            if (const char* hit = LookupLabel(ctx, title)) return hit;
        } catch (...) {
        }
        return title;
    }

    void FlushCapture() {
        if (!s_capture.load(std::memory_order_relaxed)) return;
        try {
            // Snapshot the dirty sets under the lock, then do file IO
            // outside it.
            std::vector<std::pair<std::string, std::set<std::string>>> dirty;
            {
                std::lock_guard lock(s_mutex);
                for (auto& [mod, ctx] : s_ctxByModule) {
                    if (ctx && ctx->captureDirty && !ctx->captured.empty()) {
                        dirty.emplace_back(ctx->name, ctx->captured);
                        ctx->captureDirty = false;
                    }
                }
            }
            for (auto& [name, strings] : dirty) {
                const auto dir = std::filesystem::path("Data/F4SE/Plugins") / name / "Languages";
                const auto file = dir / "captured_strings.json";

                // Merge with what a previous session captured.
                std::map<std::string, std::string> merged;
                try {
                    std::ifstream in(file, std::ios::binary);
                    if (in.is_open()) {
                        auto j = nlohmann::json::parse(in);
                        if (j.is_object()) {
                            for (auto& [k, v] : j.items()) {
                                if (v.is_string()) merged[k] = v.get<std::string>();
                            }
                        }
                    }
                } catch (...) {
                    // Unreadable previous capture: start fresh.
                }
                for (const auto& s : strings) merged.try_emplace(s, s);

                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                nlohmann::json out = nlohmann::json::object();
                for (const auto& [k, v] : merged) out[k] = v;
                std::ofstream of(file, std::ios::binary | std::ios::trunc);
                if (of.is_open()) {
                    of << out.dump(2) << '\n';
                    logger::info("AutoTranslate: capture wrote {} string(s) for '{}'",
                                 merged.size(), name);
                } else {
                    logger::warn("AutoTranslate: capture could not write '{}'",
                                 file.string());
                }
            }
        } catch (...) {
            // Capture is a dev/translator aid; never let it break menu close.
        }
    }

}
