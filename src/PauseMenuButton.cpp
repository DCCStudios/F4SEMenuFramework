#include "PauseMenuButton.h"
#include "WindowManager.h"

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
// flash.display.Loader. The SWF's ActionScript (F4SEFrameworkPause) adds the
// list row and calls `f4semf.OpenMenu()` when the player selects it.

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
        if (url.GetString() != kHostSWF) {
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

        // Build a Loader + URLRequest and load our SWF, then parent it under
        // Menu_mc so it participates in the pause menu's display list (mirrors
        // MCM's "root.mcm_loader" + "root.Menu_mc.addChild" sequence).
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
}
