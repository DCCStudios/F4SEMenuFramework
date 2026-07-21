#include "MCM/MCMPapyrusAPI.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMWidgetRenderer.h"
#include "MCM/PapyrusFunctionArgs.h"
#include "Config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>  // _ReturnAddress — identifies raw-shim callers for fault logging

#include <cstring>
#include <mutex>
#include <set>

namespace MCMPapyrusAPI {

    // Set once RegisterFunctions has successfully bound the natives.
    static std::atomic<bool> s_nativesBound{ false };

    // ------------------------------------------------------------------
    // Native implementations (script "MCM", all global/static functions).
    //
    // Global Papyrus functions receive std::monostate as the "self"
    // parameter under CommonLibF4's BSScriptUtil binding machinery.
    // Value access is routed through MCMValueProvider's raw mod-setting
    // API, which layers Config defaults under user-saved values and
    // writes to the canonical flat file Data/MCM/Settings/<ModName>.ini.
    // ------------------------------------------------------------------

    static bool IsInstalled(std::monostate) {
        // We ARE the MCM API provider in this session (registration is
        // skipped entirely when the real MCM.dll is present).
        return true;
    }

    static std::int32_t GetVersionCode(std::monostate) {
        return kVersionCode;
    }

    static void RefreshMenu(std::monostate) {
        // Real MCM re-pulls all displayed values from their sources.
        // Our equivalent: invalidate the widget renderer's cached control
        // states so the next rendered frame re-reads every value.
        MCMWidgetRenderer::InvalidateAllStates();
    }

    static std::int32_t GetModSettingInt(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        if (!raw.has_value()) return -1;  // matches real MCM's "missing setting" result
        try { return std::stoi(*raw); } catch (...) { return -1; }
    }

    static bool GetModSettingBool(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        if (!raw.has_value()) return false;
        return *raw != "0" && !raw->empty();  // real MCM treats any non-"0" as true
    }

    static float GetModSettingFloat(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        if (!raw.has_value()) return -1.0f;
        try { return std::stof(*raw); } catch (...) { return -1.0f; }
    }

    static std::string GetModSettingString(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        return raw.value_or(std::string{});
    }

    static void SetModSettingInt(std::monostate, std::string a_modName, std::string a_setting, std::int32_t a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, std::to_string(a_value));
    }

    static void SetModSettingBool(std::monostate, std::string a_modName, std::string a_setting, bool a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, a_value ? "1" : "0");
    }

    static void SetModSettingFloat(std::monostate, std::string a_modName, std::string a_setting, float a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, std::to_string(a_value));
    }

    static void SetModSettingString(std::monostate, std::string a_modName, std::string a_setting, std::string a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, a_value);
    }

    // ------------------------------------------------------------------
    // f4se-ABI compatibility layer for the registered natives
    //
    // Some native plugins do NOT call MCM's Papyrus natives through the VM.
    // Instead they locate the registered IFunction object for e.g.
    // "MCM.GetModSettingFloat" in the VM's function table, cast it to the
    // classic f4se `NativeFunction` layout, read the raw C callback pointer
    // stored at offset 0x50 (`NativeFunction::m_callback` in f4se's
    // PapyrusNativeFunctions.h), and call it directly from their own threads.
    // LooksMenuTempScroll does exactly this on every MenuOpenCloseEvent.
    //
    // That works against the real mcm.dll (built on f4se, raw pointer at
    // 0x50). It CRASHED against our CommonLibF4 BSScriptUtil natives: those
    // store a std::function at 0x50, whose first 8 bytes are an MSVC
    // _Func_impl_no_alloc vtable pointer — the extractor called that vtable
    // address as code (crash-2026-07-17-15-21-44: "tried to execute memory"
    // at our _Func_impl_no_alloc vftable symbol, caller LooksMenuTempScroll).
    //
    // Fix: register the MCM natives through a custom NativeFunctionBase
    // subclass that is layout-compatible with f4se's NativeFunction — a real,
    // callable raw shim pointer sits at offset 0x50 — while normal Papyrus
    // calls still dispatch through MarshallAndDispatch as usual.
    //
    // ABI notes for the raw shims (verified against f4se sources):
    //  - f4se's BSFixedString is StringCache::Ref { Entry* } — trivially
    //    copyable, 8 bytes → passed AND returned in registers. CommonLibF4's
    //    BSFixedString has acquire/release copy semantics (→ hidden-reference
    //    ABI), so shims use RawFixedString, a trivially-copyable pointer
    //    mirror, and convert at the boundary.
    //  - first parameter is f4se's StaticFunctionTag* — always null/ignored.
    //  - shims can be invoked from ANY thread (LooksMenuTempScroll calls from
    //    the UI job thread); MCMValueProvider's raw API is mutex-guarded.
    // ------------------------------------------------------------------

    // Register-passed stand-in for f4se's StringCache::Ref / BSFixedString.
    struct RawFixedString {
        void* entry = nullptr;  // BSStringPool entry pointer
    };
    static_assert(sizeof(RawFixedString) == 0x8 && std::is_trivially_copyable_v<RawFixedString>);
    static_assert(sizeof(RE::BSFixedString) == 0x8);

    // Read the text of a pool entry WITHOUT touching its refcount (we don't
    // own the caller's reference). BSFixedString is a single entry pointer,
    // so alias one in a buffer and never run its destructor.
    static std::string RawToString(RawFixedString a_raw) {
        if (!a_raw.entry) return {};
        alignas(RE::BSFixedString) std::byte buf[sizeof(RE::BSFixedString)];
        std::memcpy(buf, &a_raw.entry, sizeof(a_raw.entry));
        const auto& alias = *reinterpret_cast<const RE::BSFixedString*>(buf);
        return std::string{ static_cast<std::string_view>(alias) };
    }

    // Create a pool entry holding one reference and hand the raw pointer to
    // the caller — the classic f4se "return BSFixedString by value" transfer
    // convention (the receiving plugin releases it, exactly as with real MCM).
    static RawFixedString StringToRawTransfer(const std::string& a_str) {
        alignas(RE::BSFixedString) std::byte buf[sizeof(RE::BSFixedString)];
        ::new (static_cast<void*>(buf)) RE::BSFixedString(a_str);  // acquires; intentionally never destroyed here
        RawFixedString out;
        std::memcpy(&out.entry, buf, sizeof(out.entry));
        return out;
    }

    // --- Crash guard for the raw shims ---
    // The shims are invoked by FOREIGN code that extracted our callback
    // pointer and calls it with whatever ABI assumptions it was built with,
    // on whatever thread it happens to be on. Two real crashes came from
    // exactly this (a std::function vtable executed as code; a hidden
    // return slot left unwritten). We can match the known f4se convention,
    // but we cannot control what the NEXT mod assumes — so every shim runs
    // its real work under SEH: a faulting call is logged ONCE per (function,
    // caller module) with the offending DLL's name, and the shim returns the
    // real MCM's "missing setting" default instead of crashing the game.

    // Resolves the caller's module and logs the fault (throttled so a
    // per-frame caller can't flood the log). Called from SEH handlers only.
    static void LogRawShimFault(const char* a_fn, void* a_caller, unsigned long a_code) {
        char modPath[MAX_PATH] = "<unknown module>";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(a_caller), &mod) && mod) {
            GetModuleFileNameA(mod, modPath, sizeof(modPath));
        }
        const char* modName = modPath;
        if (const char* slash = std::strrchr(modPath, '\\')) modName = slash + 1;

        static std::mutex s_mutex;
        static std::set<std::string> s_seen;
        {
            std::lock_guard lock(s_mutex);
            if (!s_seen.insert(std::string(a_fn) + "|" + modName).second) return;  // already reported
        }

        const std::uintptr_t offset =
            mod ? reinterpret_cast<std::uintptr_t>(a_caller) - reinterpret_cast<std::uintptr_t>(mod) : 0;
        logger::error(
            "[MCMPapyrusAPI] Exception 0x{:08X} inside raw MCM.{} called directly by {}+0x{:X} — "
            "the caller's ABI assumptions don't match; returned a safe default instead of crashing. "
            "That mod reads MCM settings natively and may misbehave without the real MCM installed.",
            a_code, a_fn, modName, offset);
    }

    // C++ bodies of the shims. These allocate / use destructors, so they must
    // live OUTSIDE the SEH frames (MSVC C2712: no object unwinding in a
    // function with __try). The SEH wrappers below contain only POD locals.
    static std::int32_t Impl_GetModSettingInt(RawFixedString m, RawFixedString s) {
        return GetModSettingInt({}, RawToString(m), RawToString(s));
    }
    static bool Impl_GetModSettingBool(RawFixedString m, RawFixedString s) {
        return GetModSettingBool({}, RawToString(m), RawToString(s));
    }
    static float Impl_GetModSettingFloat(RawFixedString m, RawFixedString s) {
        return GetModSettingFloat({}, RawToString(m), RawToString(s));
    }
    static void Impl_GetModSettingString(RawFixedString* r, RawFixedString m, RawFixedString s) {
        *r = StringToRawTransfer(GetModSettingString({}, RawToString(m), RawToString(s)));
    }
    static void Impl_SetModSettingInt(RawFixedString m, RawFixedString s, std::int32_t v) {
        SetModSettingInt({}, RawToString(m), RawToString(s), v);
    }
    static void Impl_SetModSettingBool(RawFixedString m, RawFixedString s, bool v) {
        SetModSettingBool({}, RawToString(m), RawToString(s), v);
    }
    static void Impl_SetModSettingFloat(RawFixedString m, RawFixedString s, float v) {
        SetModSettingFloat({}, RawToString(m), RawToString(s), v);
    }
    static void Impl_SetModSettingString(RawFixedString m, RawFixedString s, RawFixedString v) {
        SetModSettingString({}, RawToString(m), RawToString(s), RawToString(v));
    }

    // Raw shims with the exact signatures a direct caller expects after
    // extracting m_callback from the equivalent real-MCM registration.
    // IsInstalled / GetVersionCode return compile-time constants and touch no
    // caller-supplied pointers, so they need no guard.
    static bool Raw_IsInstalled(void*) { return IsInstalled({}); }
    static std::int32_t Raw_GetVersionCode(void*) { return GetVersionCode({}); }

    static void Raw_RefreshMenu(void*) {
        void* caller = _ReturnAddress();
        __try {
            RefreshMenu({});
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("RefreshMenu", caller, GetExceptionCode());
        }
    }
    static std::int32_t Raw_GetModSettingInt(void*, RawFixedString m, RawFixedString s) {
        void* caller = _ReturnAddress();
        __try {
            return Impl_GetModSettingInt(m, s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("GetModSettingInt", caller, GetExceptionCode());
            return -1;  // real MCM's "missing setting" result
        }
    }
    static bool Raw_GetModSettingBool(void*, RawFixedString m, RawFixedString s) {
        void* caller = _ReturnAddress();
        __try {
            return Impl_GetModSettingBool(m, s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("GetModSettingBool", caller, GetExceptionCode());
            return false;
        }
    }
    static float Raw_GetModSettingFloat(void*, RawFixedString m, RawFixedString s) {
        void* caller = _ReturnAddress();
        __try {
            return Impl_GetModSettingFloat(m, s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("GetModSettingFloat", caller, GetExceptionCode());
            return -1.0f;
        }
    }
    // GetModSettingString returns BSFixedString by value — and old f4se's
    // StringCache::Ref has USER-DEFINED CONSTRUCTORS, so MSVC x64 returns it
    // through a hidden caller-allocated slot (pointer in RCX) even though it
    // is only 8 bytes; every argument shifts one register right:
    //   RCX = &result, RDX = base, R8 = modName, R9 = setting
    // Verified against LooksMenuTempScroll's disassembly (its call site does
    // `lea rcx,[rsp+58h]` then reads the entry pointer back from that slot).
    // Returning the trivially-copyable RawFixedString directly would use
    // register-return and leave the caller's slot uninitialized — that was
    // crash-2026-07-17-16-48-33 (garbage entry pointer dereference). So the
    // shim spells the hidden-pointer convention out explicitly.
    static RawFixedString* Raw_GetModSettingString(RawFixedString* a_result, void* /*base*/,
                                                   RawFixedString m, RawFixedString s) {
        void* caller = _ReturnAddress();
        __try {
            Impl_GetModSettingString(a_result, m, s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("GetModSettingString", caller, GetExceptionCode());
            // Null out the result slot so the caller's null check (which the
            // f4se convention makes it responsible for) takes the safe path.
            // Guarded separately: a_result itself may be the bad pointer.
            __try {
                if (a_result) a_result->entry = nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return a_result;  // MSVC convention: hidden-pointer returns also place the slot address in RAX
    }
    static void Raw_SetModSettingInt(void*, RawFixedString m, RawFixedString s, std::int32_t v) {
        void* caller = _ReturnAddress();
        __try {
            Impl_SetModSettingInt(m, s, v);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("SetModSettingInt", caller, GetExceptionCode());
        }
    }
    static void Raw_SetModSettingBool(void*, RawFixedString m, RawFixedString s, bool v) {
        void* caller = _ReturnAddress();
        __try {
            Impl_SetModSettingBool(m, s, v);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("SetModSettingBool", caller, GetExceptionCode());
        }
    }
    static void Raw_SetModSettingFloat(void*, RawFixedString m, RawFixedString s, float v) {
        void* caller = _ReturnAddress();
        __try {
            Impl_SetModSettingFloat(m, s, v);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("SetModSettingFloat", caller, GetExceptionCode());
        }
    }
    static void Raw_SetModSettingString(void*, RawFixedString m, RawFixedString s, RawFixedString v) {
        void* caller = _ReturnAddress();
        __try {
            Impl_SetModSettingString(m, s, v);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogRawShimFault("SetModSettingString", caller, GetExceptionCode());
        }
    }

    // NativeFunction whose data layout matches f4se's: the raw shim pointer
    // is the FIRST member after NativeFunctionBase (sizeof == 0x50), i.e. at
    // offset 0x50 where f4se's NativeFunction keeps m_callback. The vtable is
    // also slot-compatible (f4se HasCallback/Run == our HasStub(0x15)/
    // MarshallAndDispatch(0x16)). VM dispatch goes through the std::function
    // stub exactly like BSScriptUtil::NativeFunction does.
    template <class R, class... Args>
    class F4SECompatNativeFunction final : public RE::BSScript::NF_util::NativeFunctionBase {
    public:
        using CppFn = R (*)(std::monostate, Args...);

        F4SECompatNativeFunction(std::string_view a_object, std::string_view a_function,
                                 CppFn a_func, void* a_rawShim) :
            NativeFunctionBase(a_object, a_function, sizeof...(Args), /*isStatic=*/true, /*isLatent=*/false),
            m_callback(a_rawShim),
            _stub(a_func) {
            std::size_t i = 0;
            ((descTable.entries[i++].second =
                  RE::BSScript::detail::GetTypeInfo<RE::BSScript::detail::decay_t<Args>>().value_or(nullptr)),
                ...);
            retType = RE::BSScript::detail::GetTypeInfo<RE::BSScript::detail::decay_t<R>>().value_or(nullptr);
        }

        bool HasStub() const override { return static_cast<bool>(_stub); }

        bool MarshallAndDispatch(RE::BSScript::Variable& a_self,
                                 RE::BSScript::Internal::VirtualMachine& a_vm,
                                 std::uint32_t a_stackID,
                                 RE::BSScript::Variable& a_retVal,
                                 const RE::BSScript::StackFrame& a_stackFrame) const override {
            a_retVal = nullptr;
            const auto stack = a_stackFrame.parent;
            if (!stack) {
                assert(false);
                logger::error("[MCMPapyrusAPI] native function called without relevant stack");
                return false;
            }

            const auto invoke = [&]() {
                return RE::BSScript::detail::DispatchHelper<false, std::monostate, Args...>(
                    a_self, a_vm, a_stackID, a_stackFrame, *stack, _stub,
                    std::index_sequence_for<Args...>{});
            };

            if constexpr (!std::same_as<R, void>) {
                RE::BSScript::PackVariable(a_retVal, invoke());
            } else {
                invoke();
            }
            return true;
        }

        // members — ORDER MATTERS: m_callback must land at offset 0x50
        void* m_callback;                              // 50 — f4se NativeFunction::m_callback mirror
        std::function<R(std::monostate, Args...)> _stub;  // 58 — VM dispatch target
    };

    // Compile-time proof the raw pointer sits where f4se readers expect it.
    static_assert(offsetof(F4SECompatNativeFunction<bool>, m_callback) == 0x50,
        "m_callback must sit at offset 0x50 to match f4se's NativeFunction layout");

    // Bind one native with both dispatch paths wired up.
    template <class R, class... Args>
    static void BindCompat(RE::BSScript::IVirtualMachine* a_vm,
                           const char* a_object, const char* a_function,
                           R (*a_cppFn)(std::monostate, Args...), void* a_rawShim,
                           std::optional<bool> a_taskletCallable = std::nullopt) {
        const bool ok = a_vm->BindNativeMethod(
            new F4SECompatNativeFunction<R, Args...>(a_object, a_function, a_cppFn, a_rawShim));
        if (!ok) {
            logger::warn("[MCMPapyrusAPI] failed to register '{}.{}'", a_object, a_function);
            return;
        }
        if (a_taskletCallable) {
            a_vm->SetCallableFromTasklets(a_object, a_function, *a_taskletCallable);
        }
    }

    // ------------------------------------------------------------------
    // Registration
    // ------------------------------------------------------------------

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) return false;

        // If the real MCM.dll is loaded in this process, its plugin registers
        // the exact same natives. Binding ours as well would fight over the
        // same function slots — defer to the real MCM entirely.
        // (This callback runs when the VM initializes, which is after ALL
        // F4SE plugins' Load functions have run, so the check is reliable.)
        if (GetModuleHandleA("mcm.dll") != nullptr) {
            logger::info("[MCMPapyrusAPI] Real MCM.dll detected — skipping MCM native registration (real MCM is authoritative)");
            return true;
        }

        // Bind every function declared in MCM.psc, f4se-layout-compatible so
        // plugins that extract the raw callback (LooksMenuTempScroll et al.)
        // keep working. taskletCallable=true for the read-only queries (same
        // as real MCM's kFunctionFlag_NoWait on IsInstalled/GetVersionCode);
        // setters must run on the VM thread.
        BindCompat(a_vm, "MCM", "IsInstalled", IsInstalled, (void*)&Raw_IsInstalled, true);
        BindCompat(a_vm, "MCM", "GetVersionCode", GetVersionCode, (void*)&Raw_GetVersionCode, true);
        BindCompat(a_vm, "MCM", "RefreshMenu", RefreshMenu, (void*)&Raw_RefreshMenu, true);
        BindCompat(a_vm, "MCM", "GetModSettingInt", GetModSettingInt, (void*)&Raw_GetModSettingInt, true);
        BindCompat(a_vm, "MCM", "GetModSettingBool", GetModSettingBool, (void*)&Raw_GetModSettingBool, true);
        BindCompat(a_vm, "MCM", "GetModSettingFloat", GetModSettingFloat, (void*)&Raw_GetModSettingFloat, true);
        BindCompat(a_vm, "MCM", "GetModSettingString", GetModSettingString, (void*)&Raw_GetModSettingString, true);
        BindCompat(a_vm, "MCM", "SetModSettingInt", SetModSettingInt, (void*)&Raw_SetModSettingInt);
        BindCompat(a_vm, "MCM", "SetModSettingBool", SetModSettingBool, (void*)&Raw_SetModSettingBool);
        BindCompat(a_vm, "MCM", "SetModSettingFloat", SetModSettingFloat, (void*)&Raw_SetModSettingFloat);
        BindCompat(a_vm, "MCM", "SetModSettingString", SetModSettingString, (void*)&Raw_SetModSettingString);

        s_nativesBound.store(true);
        logger::info("[MCMPapyrusAPI] MCM script natives registered (f4se-layout compatible) — F4SE Menu Framework is acting as the MCM API provider");
        return true;
    }

    bool AreNativesBound() {
        return s_nativesBound.load();
    }

    // ------------------------------------------------------------------
    // External event dispatch
    //
    // Mirrors how the real MCM fires script events: F4SE keeps a registry of
    // RegisterForExternalEvent registrations per event name; for each
    // registrant we dispatch a Papyrus method call on the registered handle
    // with the registered callback name. Argument packing goes through the
    // same BSTThreadScrapFunction machinery as our action dispatch.
    // ------------------------------------------------------------------

    // Payload carried through the C-style registrant functor.
    struct EventArgs {
        std::vector<std::string> args;  // all MCM event args are strings
    };

    // Dispatches one registrant. Called by F4SE once per registration.
    static void F4SEAPI RegistrantFunctor(std::uint64_t a_handle, const char* a_scriptName,
                                          const char* a_callbackName, void* a_data) {
        auto* payload = static_cast<EventArgs*>(a_data);
        if (!payload || !a_scriptName || !a_callbackName) return;

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM || !gameVM->GetVM()) return;
        auto* vm = gameVM->GetVM().get();

        // Pack the string args (0, 1 or 2 of them) into a scrap function.
        RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> scrapFunc;
        switch (payload->args.size()) {
            case 0:
                scrapFunc = (PapyrusFunctionArgs::FunctionArgs<>{ vm }).get();
                break;
            case 1:
                scrapFunc = (PapyrusFunctionArgs::FunctionArgs<RE::BSFixedString>{
                    vm, RE::BSFixedString(payload->args[0].c_str()) }).get();
                break;
            default:
                scrapFunc = (PapyrusFunctionArgs::FunctionArgs<RE::BSFixedString, RE::BSFixedString>{
                    vm, RE::BSFixedString(payload->args[0].c_str()),
                    RE::BSFixedString(payload->args[1].c_str()) }).get();
                break;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
        vm->DispatchMethodCall(a_handle,
            RE::BSFixedString(a_scriptName),
            RE::BSFixedString(a_callbackName),
            scrapFunc, nullCallback);
    }

    // Fires a single external event name to all its registrants.
    static void FireExternalEvent(const std::string& eventName, EventArgs& payload) {
        auto* papyrus = F4SE::GetPapyrusInterface();
        if (!papyrus) return;
        papyrus->GetExternalEventRegistrations(eventName, &payload, RegistrantFunctor);
    }

    void DispatchSettingChanged(const std::string& modName, const std::string& controlId) {
        // Real MCM only sends the event when the control has an id.
        if (controlId.empty()) return;

        EventArgs payload;
        payload.args = { modName, controlId };
        // Both the unfiltered and the "|ModName" filtered event receive
        // (modName, id) — verified against the MCM AS3 source (OptionsList.as).
        FireExternalEvent("OnMCMSettingChange", payload);
        FireExternalEvent("OnMCMSettingChange|" + modName, payload);
        logger::debug("[MCMPapyrusAPI] OnMCMSettingChange fired (mod='{}', id='{}')", modName, controlId);
    }

    void DispatchMenuOpen(const std::string& modName) {
        // Despite MCM.psc's doc comment claiming OnMCMMenuOpen(string modName),
        // the shipped MCM.swf sends BOTH the unfiltered and the "|ModName"
        // variant with NO arguments (MCM_Menu.as: SendExternalEvent(name) with
        // nothing else). The Papyrus VM rejects calls whose argument count
        // doesn't match the handler, so passing modName here silently broke
        // zero-parameter handlers written against real MCM's actual behavior
        // (e.g. Game Visuals Configuration Menu's OnMCMMenuOpen()).
        EventArgs payload;  // intentionally empty — match the real MCM
        FireExternalEvent("OnMCMMenuOpen", payload);
        FireExternalEvent("OnMCMMenuOpen|" + modName, payload);
        logger::debug("[MCMPapyrusAPI] OnMCMMenuOpen fired (mod='{}')", modName);
    }

    void DispatchMenuClose(const std::string& modName) {
        // Real MCM only ever sends the filtered "OnMCMMenuClose|<mod>" variant
        // (there is no unfiltered send site in the shipped SWF), and it carries
        // no arguments. Mirror that exactly so registrants behave identically
        // whether real MCM or this framework is the provider.
        EventArgs payload;  // intentionally empty — match the real MCM
        FireExternalEvent("OnMCMMenuClose|" + modName, payload);
        logger::debug("[MCMPapyrusAPI] OnMCMMenuClose| fired (mod='{}')", modName);
    }

    void DispatchMCMOpen() {
        EventArgs payload;  // legacy event carries no arguments (MCM_Main.as)
        FireExternalEvent("OnMCMOpen", payload);
    }

    void DispatchMCMClose() {
        EventArgs payload;
        FireExternalEvent("OnMCMClose", payload);
    }

} // namespace MCMPapyrusAPI
