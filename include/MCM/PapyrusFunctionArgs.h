/*
MIT License

Copyright (c) 2022 shad0wshayd3

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

// Adapted from shad0wshayd3's FunctionArgs implementation used by NAF and LighthousePapyrusExtender.
// Provides the ability to construct BSTThreadScrapFunction objects needed for Papyrus VM dispatch.

// IStackCallbackFunctor is only forward-declared in our CommonLibF4 fork.
// We need a minimal definition so BSTSmartPointer's template can instantiate.
// This matches the PostNG definition: inherits BSIntrusiveRefCounted, abstract vtable.
namespace RE::BSScript
{
	class __declspec(novtable) alignas(0x08) IStackCallbackFunctor :
		public BSIntrusiveRefCounted
	{
	public:
		virtual ~IStackCallbackFunctor() = default;
		virtual void CallQueued() = 0;
		virtual void CallCanceled() = 0;
		virtual void StartMultiDispatch() = 0;
		virtual void EndMultiDispatch() = 0;
		virtual void operator()(BSScript::Variable) = 0;
		virtual bool CanSave() { return false; }
	};
}

namespace RE::BSScript
{
	// Wraps a Variable inside a managed array container.
	// Binary size: 0x10 (one Variable member).
	// The specialization for Variable uses a game function to populate from BSScrapArray.
	template <typename T>
	class ArrayWrapper
	{
	public:
		ArrayWrapper() = delete;
		F4_HEAP_REDEFINE_NEW(ArrayWrapper<T>);

	private:
		Variable wrappedVar;  // 00
	};
	static_assert(sizeof(ArrayWrapper<void*>) == 0x10);

	template <>
	class ArrayWrapper<BSScript::Variable>
	{
	public:
		ArrayWrapper() = delete;
		ArrayWrapper(BSScrapArray<Variable>& a_copy, IVirtualMachine& a_vm)
		{
			ReplaceArray(a_copy, a_vm);
		}

		F4_HEAP_REDEFINE_NEW(ArrayWrapper<BSScript::Variable>);

		void ReplaceArray(BSScrapArray<Variable>& a_copy, IVirtualMachine& a_vm)
		{
			using func_t = decltype(&ArrayWrapper::ReplaceArray);
			REL::Relocation<func_t> func{ REL::ID(445184) };
			return func(this, a_copy, a_vm);
		}

	private:
		Variable wrappedVar;  // 00
	};
	static_assert(sizeof(ArrayWrapper<Variable>) == 0x10);
}

namespace PapyrusFunctionArgs
{
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

		// Base class matching the binary layout expected by the game's CreateThreadScrapFunction.
		// Layout: [ArrayWrapper* args (8 bytes)] [IVirtualMachine* vm (8 bytes)] = 0x10
		class FunctionArgsBase
		{
		public:
			FunctionArgsBase() = delete;

			FunctionArgsBase(RE::BSScript::IVirtualMachine* a_vm) :
				vm(a_vm)
			{}

		protected:
			RE::BSScript::ArrayWrapper<RE::BSScript::Variable>* args;  // 00
			RE::BSScript::IVirtualMachine* vm;                         // 08
		};

		static_assert(sizeof(FunctionArgsBase) == 0x10);

		// Calls the game's internal function that constructs a BSTThreadScrapFunction
		// from our FunctionArgsBase object. REL::ID(69733) is the address of this function.
		inline RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)>
			CreateThreadScrapFunction(FunctionArgsBase& a_args)
		{
			using func_t = decltype(&detail::CreateThreadScrapFunction);
			REL::Relocation<func_t> func{ REL::ID(69733) };
			return func(a_args);
		}
	}

	// Variadic template class that packs arguments and produces a BSTThreadScrapFunction.
	// Usage:
	//   auto scrapFunc = (PapyrusFunctionArgs::FunctionArgs{ vm, arg1, arg2, ... }).get();
	//   vm->DispatchStaticCall(scriptName, funcName, scrapFunc, nullptr);
	template <class... Args>
	class FunctionArgs :
		public detail::FunctionArgsBase
	{
	public:
		FunctionArgs() = delete;

		FunctionArgs(RE::BSScript::IVirtualMachine* a_vm, Args... a_args) :
			FunctionArgsBase(a_vm)
		{
			auto scrap = detail::PackVariables(a_args...);
			args = new RE::BSScript::ArrayWrapper<RE::BSScript::Variable>(scrap, *vm);
		}

		RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> get()
		{
			return detail::CreateThreadScrapFunction(*this);
		}
	};

	static_assert(sizeof(FunctionArgs<std::monostate>) == 0x10);

	// Runtime-typed variant of FunctionArgs: accepts a pre-built
	// BSScrapArray<Variable> whose element types are only known at runtime
	// (used for MCM structured actions whose "params" come from JSON).
	class RuntimeFunctionArgs :
		public detail::FunctionArgsBase
	{
	public:
		RuntimeFunctionArgs() = delete;

		RuntimeFunctionArgs(RE::BSScript::IVirtualMachine* a_vm,
		                    RE::BSScrapArray<RE::BSScript::Variable>& a_scrap) :
			FunctionArgsBase(a_vm)
		{
			args = new RE::BSScript::ArrayWrapper<RE::BSScript::Variable>(a_scrap, *vm);
		}

		RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> get()
		{
			return detail::CreateThreadScrapFunction(*this);
		}
	};

	static_assert(sizeof(RuntimeFunctionArgs) == 0x10);

	// --- Convenience helpers for dispatching Papyrus calls ---

	// Call a global/static Papyrus function with no arguments (fire-and-forget)
	inline bool CallGlobalFunction(const std::string& scriptName, const std::string& funcName)
	{
		auto* vm = RE::GameVM::GetSingleton();
		if (!vm || !vm->GetVM()) return false;

		auto* vmRaw = vm->GetVM().get();
		auto scrapFunc = (FunctionArgs<>{ vmRaw }).get();

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
		return vmRaw->DispatchStaticCall(
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			scrapFunc,
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

		auto scrapFunc = (FunctionArgs<>{ vmRaw }).get();
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;

		bool result = vmRaw->DispatchMethodCall(
			handle,
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			scrapFunc,
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
		auto scrapFunc = (FunctionArgs<Args...>{ vmRaw, a_args... }).get();

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
		return vmRaw->DispatchStaticCall(
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			scrapFunc,
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

		auto scrapFunc = (FunctionArgs<Args...>{ vmRaw, a_args... }).get();
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;

		bool result = vmRaw->DispatchMethodCall(
			handle,
			RE::BSFixedString(scriptName.c_str()),
			RE::BSFixedString(funcName.c_str()),
			scrapFunc,
			nullCallback);

		handlePolicy.ReleaseHandle(handle);
		return result;
	}
}
