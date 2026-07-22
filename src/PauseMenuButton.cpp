#include "PauseMenuButton.h"
#include "WindowManager.h"

#include "RE/B/BSTEvent.h"
#include "RE/M/MenuOpenCloseEvent.h"
#include "RE/U/UI.h"

// Pause-menu button integration.
//
// This mirrors the well-proven injection pattern used by the real MCM
// (PluginTemplate/f4mcm/src/ScaleformMCM.cpp, RegisterScaleform) but expressed
// against the modern CommonLibF4 Scaleform API:
//
//   * MCM's `GFxMovieRoot`  -> `RE::Scaleform::GFx::Movie::asMovieRoot`
//     (an `ASMovieRootBase*` exposing CreateObject / CreateFunction /
//      CreateString / SetVariable / GetVariable / Invoke).
//   * MCM's `GFxValue`      -> `RE::Scaleform::GFx::Value`.
//   * MCM's `GFxFunctionHandler::Invoke` -> `FunctionHandler::Call`.
//
// When the game loads Interface/MainMenu.swf (used for both the title menu and
// the in-game pause menu) F4SE invokes our callback. We register a native code
// object on the movie root as `f4semf` and load our own SWF into the menu via a
// flash.display.Loader. The SWF's ActionScript (F4SEFrameworkPause) attaches the
// list press handler and can also insert the row.
//
// Separately, on PauseMenu open we insert the list row from C++ (PhotoMode-style)
// so the "F4SE FRAMEWORK" entry appears as soon as the menu is up — without
// waiting for F4SEFramework.swf to finish loading. Placement is top-of-list only;
// we do not move the row relative to MCM.

namespace
{
    using RE::Scaleform::GFx::FunctionHandler;
    using RE::Scaleform::GFx::Movie;
    using RE::Scaleform::GFx::Value;

    // Filename of the SWF we ship + inject. Lives in Data/Interface/ so the
    // engine's Scaleform loader can resolve it by bare name (same convention as
    // MCM loading "MCM.swf").
    constexpr auto kSWFName = "F4SEFramework.swf";

    // Host movie we inject into. This is the pause/main menu movie; the SWF's
    // own ActionScript gates on PauseMode so the row only appears in the
    // in-game pause menu, not the title screen.
    constexpr auto kHostSWF = "Interface/MainMenu.swf";

    // Must match F4SEFrameworkPause.as — sentinel list index + label.
    constexpr double kEntryIndex = 500.0;
    constexpr auto kEntryText = "F4SE FRAMEWORK";

    // Native function exposed to our SWF as `f4semf.OpenMenu()`.
    class OpenMenuFunc : public FunctionHandler
    {
    public:
        void Call(const Params& a_params) override
        {
            logger::info("[PauseMenuButton] OpenMenu() invoked from SWF");

            // Open the already-existing ImGui overlay ON TOP of the pause menu —
            // we deliberately do NOT dismiss the PauseMenu:
            //  * Input can't leak to it: while any overlay window is open the
            //    WndProc hook swallows all window messages, GameLock sets
            //    ControlMap::ignoreKeyboardMouse, and the gamepad Poll hook
            //    clears the device state each frame — so the list underneath
            //    can't navigate or activate (no accidental Save/Quit).
            //  * The game stays paused by the pause menu itself.
            //  * MCM's `root.mcm` Scaleform object lives on this movie, so
            //    MCMLiveSync can push hotkey rebinds into the running MCM
            //    immediately instead of waiting for the menu to reopen.
            //  * Closing the overlay drops the player back on the pause menu
            //    they came from — one ESC closes us, the next unpauses.
            WindowManager::Open();

            if (a_params.retVal) {
                *a_params.retVal = true;
            }
        }
    };

    // Insert (or confirm) the F4SE FRAMEWORK row on the pause list. Returns true
    // when the row is present afterwards. Safe to call repeatedly.
    bool InjectListEntry(Movie* a_movie)
    {
        if (!a_movie) {
            return false;
        }

        // Only the in-game pause menu (not the title MainMenu).
        Value pauseMode;
        if (!a_movie->GetVariable(std::addressof(pauseMode), "root.Menu_mc.PauseMode") ||
            !pauseMode.IsBoolean() || !pauseMode.GetBoolean()) {
            return false;
        }

        Value entryList;
        if (!a_movie->GetVariable(std::addressof(entryList),
                "root.Menu_mc.MainPanel_mc.List_mc.entryList") ||
            !entryList.IsArray()) {
            return false;
        }

        // Already present — leave it where it is (no MCM-relative reshuffle).
        const auto size = entryList.GetArraySize();
        for (std::uint32_t i = 0; i < size; ++i) {
            Value entry;
            if (!entryList.GetElement(i, std::addressof(entry)) || !entry.IsObject()) {
                continue;
            }
            Value indexVal;
            if (entry.GetMember("index", std::addressof(indexVal)) && indexVal.IsNumber() &&
                indexVal.GetNumber() == kEntryIndex) {
                return true;
            }
        }

        Value newEntry;
        a_movie->CreateObject(std::addressof(newEntry));
        newEntry.SetMember("text", kEntryText);
        newEntry.SetMember("index", kEntryIndex);

        // Always insert at the top. MCM may land above or below us depending on
        // load order — we deliberately do not chase or reposition relative to it.
        Value spliceArgs[3];
        spliceArgs[0] = 0.0;
        spliceArgs[1] = 0.0;
        spliceArgs[2] = newEntry;
        if (!entryList.Invoke("splice", nullptr, spliceArgs, 3)) {
            logger::warn("[PauseMenuButton] entryList.splice failed");
            return false;
        }

        Value list;
        if (a_movie->GetVariable(std::addressof(list), "root.Menu_mc.MainPanel_mc.List_mc")) {
            list.Invoke("InvalidateData");
        }

        logger::info("[PauseMenuButton] List row injected (index {})", static_cast<int>(kEntryIndex));
        return true;
    }

    // F4SE Scaleform callback. Called for every movie the game loads; we act
    // only on the pause/main menu movie.
    bool ScaleformCallback(Movie* a_view, Value* /*a_f4seRoot*/)
    {
        if (!a_view || !a_view->asMovieRoot) {
            return true;
        }
        auto* root = a_view->asMovieRoot.get();

        // Identify the movie by its source path, exactly like MCM does.
        Value url;
        if (!root->GetVariable(std::addressof(url), "root.loaderInfo.url") || !url.IsString()) {
            return true;
        }
        // GetString() returns const char* in the multi-runtime CommonLibF4
        // (the old fork returned std::string_view) — compare CONTENTS, not
        // pointers, or the check fails for every movie and the button is gone.
        if (std::string_view{ url.GetString() } != kHostSWF) {
            return true;
        }

        logger::info("[PauseMenuButton] Injecting into {}", kHostSWF);

        Value rootObj;
        if (!root->GetVariable(std::addressof(rootObj), "root")) {
            logger::warn("[PauseMenuButton] could not resolve 'root'");
            return true;
        }

        // Register the native code object: root.f4semf.OpenMenu().
        Value codeObj;
        root->CreateObject(std::addressof(codeObj));

        Value openFn;
        root->CreateFunction(std::addressof(openFn), new OpenMenuFunc());
        codeObj.SetMember("OpenMenu", openFn);

        rootObj.SetMember("f4semf", codeObj);

        // Best-effort early insert: list may not be populated yet on first
        // Scaleform registration (MCM waits one ENTER_FRAME for the same
        // reason). PauseMenu open handles the reliable path.
        InjectListEntry(a_view);

        // Build a Loader + URLRequest and load our SWF, then parent it under
        // Menu_mc so it participates in the pause menu's display list (mirrors
        // MCM's "root.mcm_loader" + "root.Menu_mc.addChild" sequence). The SWF
        // attaches the itemPress listener and can fill in the row if C++ ran
        // before the list existed.
        Value loader;
        root->CreateObject(std::addressof(loader), "flash.display.Loader");

        Value urlName;
        root->CreateString(std::addressof(urlName), kSWFName);

        Value urlRequest;
        root->CreateObject(std::addressof(urlRequest), "flash.net.URLRequest", std::addressof(urlName), 1);

        rootObj.SetMember("f4semf_loader", loader);

        if (!root->Invoke("root.f4semf_loader.load", nullptr, std::addressof(urlRequest), 1)) {
            logger::warn("[PauseMenuButton] f4semf_loader.load failed");
        }
        if (!root->Invoke("root.Menu_mc.addChild", nullptr, std::addressof(loader), 1)) {
            logger::warn("[PauseMenuButton] Menu_mc.addChild failed");
        }

        logger::info("[PauseMenuButton] Injection complete");
        return true;
    }

    // Inserts the list row the moment PauseMenu opens so appearance is not
    // gated on F4SEFramework.swf load latency (PhotoMode pattern).
    class PauseMenuOpenSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static PauseMenuOpenSink* GetSingleton()
        {
            static PauseMenuOpenSink singleton;
            return std::addressof(singleton);
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event.opening || a_event.menuName != "PauseMenu") {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto menu = ui->GetMenu("PauseMenu");
            if (menu && menu->uiMovie) {
                InjectListEntry(menu->uiMovie.get());
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        PauseMenuOpenSink() = default;
    };

    bool g_menuEventsRegistered = false;
}

namespace PauseMenuButton
{
    void Install()
    {
        const auto* scaleform = F4SE::GetScaleformInterface();
        if (!scaleform) {
            logger::warn("[PauseMenuButton] Scaleform interface unavailable; pause-menu button disabled");
            return;
        }

        if (scaleform->Register("f4semf", ScaleformCallback)) {
            logger::info("[PauseMenuButton] Registered Scaleform callback (SWF: {})", kSWFName);
        } else {
            logger::warn("[PauseMenuButton] Failed to register Scaleform callback");
        }
    }

    void RegisterMenuEvents()
    {
        if (g_menuEventsRegistered) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("[PauseMenuButton] UI unavailable; deferring PauseMenu sink");
            return;
        }

        ui->RegisterSink(PauseMenuOpenSink::GetSingleton());
        g_menuEventsRegistered = true;
        logger::info("[PauseMenuButton] Registered PauseMenu open sink (early list inject)");
    }
}
