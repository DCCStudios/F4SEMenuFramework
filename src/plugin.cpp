#include "Hooks.h"
#include "Config.h"
#include "Logger.h"
#include "UI.h"
#include "F4SEMenuFramework.h"
#include "Translations.h"
#include "WelcomeBanner.h"
#include "HotkeyManager.h"
#include "GamepadInput.h"
#include "PauseMenuButton.h"
#include "MCM/MCMRegistry.h"
#include "MCM/MCMPapyrusAPI.h"

namespace Plugin
{
    static constexpr auto NAME = "F4SEMenuFramework"sv;
    static constexpr auto VERSION = REL::Version{ 3, 1, 0 };
}

void MessageCallback(F4SE::MessagingInterface::Message* msg)
{
    if (!msg) return;

    switch (msg->type) {
    case F4SE::MessagingInterface::kGameDataReady:
    {
        Hooks::InstallInputHooks();

        // Second IAT-hook pass: patches XInputGetState imports in modules that
        // were loaded after F4SEPlugin_Load (other F4SE plugins, overlays).
        // Idempotent — already-patched entries are skipped.
        GamepadInput::InstallXInputHook();

        WelcomeBanner::Show();

        // Initialize MCM backwards compatibility system.
        // Scans for MCM mod configs, checks for native MCM conflicts,
        // parses configs, and registers translated menus.
        MCMRegistry::Init();
        break;
    }
    default:
        break;
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = 1;

    if (a_f4se->IsEditor()) {
        logger::critical("Loaded in editor, marking as incompatible");
        return false;
    }

    const auto ver = a_f4se->RuntimeVersion();
    if (ver < F4SE::RUNTIME_1_10_162) {
        logger::critical("Unsupported runtime version {}", ver.string());
        return false;
    }

    return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    SetupLog();
    logger::info("{} v{} loading", Plugin::NAME, Plugin::VERSION.string());

    F4SE::Init(a_f4se);
    F4SE::AllocTrampoline(128);

    auto* messaging = F4SE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageCallback)) {
        logger::critical("Failed to register messaging listener");
        return false;
    }

    Config::Init();

    // Register the MCM script natives (MCM.GetModSettingInt etc.) so mods that
    // call the MCM Papyrus API work without the real MCM installed. The
    // callback runs when the Papyrus VM initializes; it skips itself if the
    // real MCM.dll is loaded (its own natives take precedence). Gated on the
    // MCM compatibility toggle like the rest of the layer. Must run after
    // Config::Init() so the toggle reflects the saved INI value.
    if (Config::MCMCompatEnabled) {
        auto* papyrus = F4SE::GetPapyrusInterface();
        if (papyrus && papyrus->Register(MCMPapyrusAPI::RegisterFunctions)) {
            logger::info("MCM Papyrus native registration queued");
        } else {
            logger::warn("Failed to queue MCM Papyrus native registration");
        }
    }
    // Register the Scaleform callback that injects our pause-menu button SWF.
    // Must run during Load while the Scaleform interface is queryable.
    PauseMenuButton::Install();

    HotkeyManager::Load();
    WindowManager::MainInterface = AddWindow(UI::RenderMenuWindow);
    WindowManager::ConfigInterface = AddWindow(UI::RenderConfigWindow);
    WindowManager::MainInterface->BlockUserInput = true;
    WindowManager::ConfigInterface->BlockUserInput = true;
    Translations::Install();
    Hooks::Install();

    // Install IAT hooks on XInputGetState (all loaded modules, all XInput DLL
    // variants) so gamepad input is suppressed for the game and other mods
    // while the F4SE menu is open. A second pass runs at kGameDataReady to
    // cover late-loaded modules.
    GamepadInput::InstallXInputHook();

    logger::info("{} loaded successfully", Plugin::NAME);
    return true;
}
