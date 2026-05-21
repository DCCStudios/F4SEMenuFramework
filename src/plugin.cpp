#include "Hooks.h"
#include "Config.h"
#include "Logger.h"
#include "UI.h"
#include "F4SEMenuFramework.h"
#include "Translations.h"
#include "WelcomeBanner.h"
#include "HotkeyManager.h"

namespace Plugin
{
    static constexpr auto NAME = "F4SEMenuFramework"sv;
    static constexpr auto VERSION = REL::Version{ 3, 0, 0 };
}

void MessageCallback(F4SE::MessagingInterface::Message* msg)
{
    if (!msg) return;

    switch (msg->type) {
    case F4SE::MessagingInterface::kGameDataReady:
    {
        Hooks::InstallInputHooks();
        WelcomeBanner::Show();
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
    HotkeyManager::Load();
    WindowManager::MainInterface = AddWindow(UI::RenderMenuWindow);
    WindowManager::ConfigInterface = AddWindow(UI::RenderConfigWindow);
    WindowManager::MainInterface->BlockUserInput = true;
    WindowManager::ConfigInterface->BlockUserInput = true;
    Translations::Install();
    Hooks::Install();

    logger::info("{} loaded successfully", Plugin::NAME);
    return true;
}
