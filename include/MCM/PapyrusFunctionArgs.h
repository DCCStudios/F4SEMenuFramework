#pragma once

#include <cstring>

// Papyrus dispatch argument helpers.
//
// The VM's Dispatch*Call methods take their arguments as a
// BSTThreadScrapFunction<bool(BSScrapArray<Variable>&)> — a std::function-like
// object the VM invokes to fill its argument array. The catch is that the
// LAYOUT of that object differs per runtime:
//
//  - OG 1.10.163 was compiled with the VS2013-era toolchain, whose
//    std::function is a 32-byte object (24-byte SOO buffer + impl pointer at
//    offset 0x18). Ryan's OG-only CommonLibF4 models this as msvc::function.
//  - NG 1.10.980+ / AE 1.11.x were recompiled with a modern toolchain, whose
//    std::function is 64 bytes with the impl pointer at offset 0x38 — the
//    layout the multi-runtime CommonLibF4 fork assumes when it typedefs
//    BSTThreadScrapFunction<F> = std::function<F>.
//
// Passing a modern std::function to the OG VM makes the game read a garbage
// impl pointer out of the middle of the lambda's captures. Verified from a
// crash dump 2026-07-21: clicking NAC X's page fired an external MCM event,
// the OG VM read impl at +0x18 of our 64-byte functor and crashed on the
// bogus pointer (WER sometimes reported it as an unhandled C++ exception
// instead, when the garbage happened to be readable).
//
// So GameScrapFunction below builds the functor per runtime:
//  - OG: through the game's OWN machinery, exactly like the proven pre-3.3
//    implementation (adapted from shad0wshayd3's FunctionArgs) — engine
//    helper 445184 wraps the packed argument array in an ArrayWrapper
//    Variable, and factory 69733 constructs the VS2013-layout functor that
//    copies that array out when the VM invokes it. Both IDs verified present
//    in the 1.10.163 address library (offsets 0x1317920 / 0x13dddb0).
//  - NG/AE: a plain C++ lambda wrapped in std::function (matching ABI).
//    NOTE: NG/AE dispatch has not been runtime-verified yet, only OG has.

namespace PapyrusFunctionArgs
{
	using ScrapSignature = bool(RE::BSScrapArray<RE::BSScript::Variable>&);

	namespace detail
	{
		// Packs variadic arguments into a BSScrapArray<Variable>
		template <class... Args>
		RE::BSScrapArray<RE::BSScript::Variable>
			PackVariables(Args&&... a_args)
		{
			constexpr auto size = sizeof...(a_args);
			auto args = std::make_tuple(std::forward<Args>(a_args)...);
			RE::BSScrapArray<RE::BSScript::Variable> result{ size };
			[&]<std::size_t... p>(std::index_sequence<p...>)
			{
				((RE::BSScript::PackVariable(result.at(p), std::get<p>(args))), ...);
			}
			(std::make_index_sequence<size>{});
			return result;
		}

		// --- OG (1.10.163) ABI shims ---

		// Mirror of the game's FunctionArgsBase (what factory 69733 copies into
		// the functor it builds): ArrayWrapper<Variable>* + IVirtualMachine*.
		struct FunctionArgsBaseOG
		{
			void*                          args{ nullptr };  // ArrayWrapper<Variable>* on the game heap
			RE::BSScript::IVirtualMachine* vm{ nullptr };
		};
		static_assert(sizeof(FunctionArgsBaseOG) == 0x10);

		// VS2013-layout std::function: 24-byte small-object buffer, impl
		// pointer at 0x18. For small functors the impl pointer aims INTO the
		// buffer, so this object must never be moved after construction.
		struct alignas(8) ThreadScrapFunctionOG
		{
			std::byte storage[3 * sizeof(void*)]{};  // 00
			void*     impl{ nullptr };               // 18
		};
		static_assert(sizeof(ThreadScrapFunctionOG) == 0x20);
	}

	// Owns one VM-consumable argument functor, built for the current runtime.
	// Non-copyable/non-movable: the OG representation can be self-referential.
	// Keep the owner alive across the Dispatch*Call and pass ref()/get() by
	// reference — never copy the referenced functor into a std::function.
	class GameScrapFunction
	{
	public:
		GameScrapFunction() = default;
		GameScrapFunction(const GameScrapFunction&) = delete;
		GameScrapFunction& operator=(const GameScrapFunction&) = delete;

		// Builds the functor from an already-packed argument array. The array
		// is copied (OG: into the engine ArrayWrapper; NG/AE: into the lambda),
		// so the caller's array may die immediately after this returns.
		void Build(RE::BSScript::IVirtualMachine* a_vm,
		           RE::BSScrapArray<RE::BSScript::Variable>& a_scrap)
		{
			if (REX::FModule::IsRuntimeOG()) {
				// ArrayWrapper<Variable> is a single Variable (0x10) that the
				// engine's ReplaceArray fills with an array-typed copy of the
				// arguments. Allocate it on the GAME heap (the engine owns and
				// frees it once the dispatched stack has consumed the args) —
				// this mirrors the pre-3.3 implementation exactly.
				void* wrapper = RE::malloc(0x10);
				std::memset(wrapper, 0, 0x10);  // zeroed == default (None) Variable
				using replace_t = void(void*, RE::BSScrapArray<RE::BSScript::Variable>&, RE::BSScript::IVirtualMachine&);
				const REL::Relocation<replace_t> replaceArray{ REL::ID(445184) };
				replaceArray(wrapper, a_scrap, *a_vm);

				// Factory 69733 returns the functor by value; spell the hidden
				// sret pointer out explicitly so the engine constructs directly
				// into og_ (a compiler-introduced temporary + memcpy would break
				// the buffer-internal impl pointer).
				detail::FunctionArgsBaseOG base{ wrapper, a_vm };
				using factory_t = detail::ThreadScrapFunctionOG*(detail::ThreadScrapFunctionOG*, detail::FunctionArgsBaseOG*);
				const REL::Relocation<factory_t> factory{ REL::ID(69733) };
				factory(&og_, &base);
			} else {
				// Modern runtimes: std::function ABI matches the game.
				fn_ = [scrap = a_scrap](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
					a_out = scrap;
					return true;
				};
			}
		}

		// Reference to pass to DispatchStaticCall / DispatchMethodCall /
		// SendEvent. Only valid while this object is alive; do NOT copy the
		// result into a BSTThreadScrapFunction variable (on OG that would run
		// std::function's copy ctor over foreign-layout bytes).
		[[nodiscard]] const RE::BSTThreadScrapFunction<ScrapSignature>& ref() const
		{
			if (REX::FModule::IsRuntimeOG()) {
				return *reinterpret_cast<const RE::BSTThreadScrapFunction<ScrapSignature>*>(&og_);
			}
			return fn_;
		}

	private:
		RE::BSTThreadScrapFunction<ScrapSignature> fn_;  // NG / AE
		detail::ThreadScrapFunctionOG              og_;  // OG 1.10.163
	};

	// Variadic argument pack. Usage:
	//   PapyrusFunctionArgs::FunctionArgs<T...> fargs{ vm, arg1, ... };
	//   vm->DispatchStaticCall(scriptName, funcName, fargs.get(), nullptr);
	// The FunctionArgs object must outlive the dispatch call (get() returns a
	// reference into it).
	template <class... Args>
	class FunctionArgs
	{
	public:
		FunctionArgs() = delete;
		FunctionArgs(const FunctionArgs&) = delete;
		FunctionArgs& operator=(const FunctionArgs&) = delete;

		FunctionArgs(RE::BSScript::IVirtualMachine* a_vm, Args... a_args)
		{
			auto scrap = detail::PackVariables(std::move(a_args)...);
			fn_.Build(a_vm, scrap);
		}

		[[nodiscard]] const RE::BSTThreadScrapFunction<ScrapSignature>& get() const { return fn_.ref(); }

	private:
		GameScrapFunction fn_;
	};

	// Runtime-typed variant: accepts a pre-built BSScrapArray<Variable> whose
	// element types are only known at runtime (used for MCM structured actions
	// whose "params" come from JSON, and for external event payloads).
	class RuntimeFunctionArgs
	{
	public:
		RuntimeFunctionArgs() = delete;
		RuntimeFunctionArgs(const RuntimeFunctionArgs&) = delete;
		RuntimeFunctionArgs& operator=(const RuntimeFunctionArgs&) = delete;

		RuntimeFunctionArgs(RE::BSScript::IVirtualMachine* a_vm,
		                    RE::BSScrapArray<RE::BSScript::Variable>& a_scrap)
		{
			fn_.Build(a_vm, a_scrap);
		}

		[[nodiscard]] const RE::BSTThreadScrapFunction<ScrapSignature>& get() const { return fn_.ref(); }

	private:
		GameScrapFunction fn_;
	};

	// --- Convenience helpers for dispatching Papyrus calls ---

	// Call a global/static Papyrus function with no arguments (fire-and-forget)
	inline bool CallGlobalFunction(const std::string& scriptName, const std::string& funcName)
	{
		auto* vm = RE::GameVM::GetSingleton();
		if (!vm || !vm->GetVM()) return false;

		auto* vmRaw = vm->GetVM().get();
		FunctionArgs<> fargs{ vmRaw };

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
		return vmRaw->DispatchStaticCall(
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			fargs.get(),
			nullCallback);
	}

	// Call a method on a specific form's script with no arguments (fire-and-forget)
	template <typename T = RE::TESForm>
	inline bool CallFunctionOnForm(T* form, const std::string& scriptName, const std::string& funcName)
	{
		if (!form) return false;

		auto* vm = RE::GameVM::GetSingleton();
		if (!vm || !vm->GetVM()) return false;

		auto* vmRaw = vm->GetVM().get();
		auto& handlePolicy = vmRaw->GetObjectHandlePolicy();

		auto handle = handlePolicy.GetHandleForObject(
			static_cast<std::uint32_t>(form->GetFormType()), form);
		if (handle == handlePolicy.EmptyHandle()) return false;

		FunctionArgs<> fargs{ vmRaw };
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;

		bool result = vmRaw->DispatchMethodCall(
			handle,
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			fargs.get(),
			nullCallback);

		handlePolicy.ReleaseHandle(handle);
		return result;
	}

	// Call a global/static Papyrus function with typed arguments (fire-and-forget)
	template <class... Args>
	inline bool CallGlobalFunctionWithArgs(const std::string& scriptName, const std::string& funcName, Args... a_args)
	{
		auto* vm = RE::GameVM::GetSingleton();
		if (!vm || !vm->GetVM()) return false;

		auto* vmRaw = vm->GetVM().get();
		FunctionArgs<Args...> fargs{ vmRaw, a_args... };

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
		return vmRaw->DispatchStaticCall(
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			fargs.get(),
			nullCallback);
	}

	// Call a method on a specific form's script with typed arguments (fire-and-forget)
	template <typename T, class... Args>
	inline bool CallFunctionOnFormWithArgs(T* form, const std::string& scriptName, const std::string& funcName, Args... a_args)
	{
		if (!form) return false;

		auto* vm = RE::GameVM::GetSingleton();
		if (!vm || !vm->GetVM()) return false;

		auto* vmRaw = vm->GetVM().get();
		auto& handlePolicy = vmRaw->GetObjectHandlePolicy();

		auto handle = handlePolicy.GetHandleForObject(
			static_cast<std::uint32_t>(form->GetFormType()), form);
		if (handle == handlePolicy.EmptyHandle()) return false;

		FunctionArgs<Args...> fargs{ vmRaw, a_args... };
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;

		bool result = vmRaw->DispatchMethodCall(
			handle,
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			fargs.get(),
			nullCallback);

		handlePolicy.ReleaseHandle(handle);
		return result;
	}
}
