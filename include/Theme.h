#pragma once


class Theme {
public:
    static std::vector<std::string> GetJsonFiles();
    static void LoadJsonStyle(const std::string& path);

    // Live reload: throttled (~1/sec) scan of the Themes folder that detects
    // .json files added, removed, or overwritten in place since the last
    // call. Intended to be polled every frame while the Settings window is
    // open (see UI::RenderConfigWindow) so the caller can refresh the theme
    // list and/or reapply the active theme without requiring a game restart.
    static bool PollForChanges();
};