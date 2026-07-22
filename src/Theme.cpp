#include <imgui.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "Theme.h"
#include "Utils.h"

// Theme JSON files now live under F4SE/Plugins/F4SEMenuFramework/Themes —
// grouped with the framework's other asset folders (Fonts, Gamepad) instead
// of sitting loose in F4SE/Plugins.
static constexpr const char* kThemesFolder = "Data\\F4SE\\plugins\\F4SEMenuFramework\\Themes";

// std::filesystem::path::string() converts through the process ANSI code
// page on Windows and throws std::system_error (not filesystem_error) if a
// theme filename can't be represented there. u8string() is UTF-8 and never
// throws for a valid path.
static std::string ThemePathUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

static ImVec4 HexToImVec4(const std::string& hex) {
    unsigned int v = 0;
    std::stringstream ss;
    ss << std::hex << hex.substr(1);
    ss >> v;
    float a = ((v >> 0) & 0xFF) / 255.0f;
    float b = ((v >> 8) & 0xFF) / 255.0f;
    float g = ((v >> 16) & 0xFF) / 255.0f;
    float r = ((v >> 24) & 0xFF) / 255.0f;
    return ImVec4(r, g, b, a);
}

std::vector<std::string> Theme::GetJsonFiles() {
    std::string folder = kThemesFolder;
    std::vector<std::string> out;
    if (!std::filesystem::exists(folder)) {
        return out;
    }
    try {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.is_regular_file()) {
                auto path = entry.path();
                if (path.extension() == ".json") {
                    out.push_back(Utils::toUpperCase(ThemePathUtf8(path.stem()).c_str()));
                }
            }
        }
    } catch (const std::system_error& e) {
        logger::warn("[Theme] Theme directory scan aborted: {}", e.what());
    }
    return out;
}

void Theme::LoadJsonStyle(const std::string& path) {
    const std::string fullPath = std::string(kThemesFolder) + "\\" + path + ".json";

    // A theme file that was dropped in (or edited) moments ago can briefly
    // fail to open — confirmed in practice: selecting a theme within ~2
    // seconds of copying its file into the Themes folder while the game is
    // running logged "Could not open theme file" here, yet the identical
    // file opened fine on the next boot. That points at a transient lock
    // (antivirus real-time scanning a newly-created file is the usual
    // culprit on Windows) rather than a missing/misnamed file. Retry a
    // few times with a short back-off before giving up — cheap when the
    // file is already stable (the common case exits on attempt 1), and the
    // difference between "works now" and "works after a restart" when it's
    // not.
    std::ifstream f;
    constexpr int kMaxOpenAttempts = 5;
    constexpr auto kOpenRetryDelay = std::chrono::milliseconds(40);
    int openAttempts = 0;
    for (; openAttempts < kMaxOpenAttempts; ++openAttempts) {
        f.open(fullPath);
        if (f.good()) break;
        f.close();
        std::this_thread::sleep_for(kOpenRetryDelay);
    }
    if (!f.good()) {
        // This used to fail completely silently — no log, no visual change,
        // nothing to go on. That made a wrong `path` (e.g. a case mismatch,
        // or a name that doesn't match any file after a live-reload list
        // refresh) indistinguishable from "theme selected but did nothing".
        logger::warn("[Theme] Could not open theme file '{}' after {} attempt(s) — theme not applied.", fullPath,
                    kMaxOpenAttempts);
        return;
    }
    if (openAttempts > 0) {
        logger::info("[Theme] '{}' took {} attempt(s) to open (transient lock) before succeeding.", path,
                    openAttempts + 1);
    }

    // Always reset to a known-good baseline first. If the JSON below is
    // malformed (or a value has the wrong type), everything past that point
    // is caught and skipped — the user ends up with plain StyleColorsDark
    // instead of a crash. This matters a lot now that live reload calls
    // this function automatically the instant a theme file's timestamp
    // changes: a text editor can briefly write a half-saved / invalid JSON
    // file, and that must never be able to bring down the game.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);

    // Parsing and applying the theme's overrides can throw at any point —
    // malformed JSON (json::parse_error), a value of the wrong type
    // (json::type_error, e.g. a string where a number was expected), or an
    // empty/invalid hex color string in HexToImVec4 (std::out_of_range).
    // Catch broadly and bail out: whatever was already applied above (or
    // earlier in this loop) stands, the rest silently falls back to the
    // StyleColorsDark baseline instead of tearing down the game.
    try {
        nlohmann::json j;
        f >> j;

        ImGuiStyle& s = ImGui::GetStyle();

        if (j.contains("Alpha")) s.Alpha = j["Alpha"];
        if (j.contains("DisabledAlpha")) s.DisabledAlpha = j["DisabledAlpha"];
        if (j.contains("WindowPadding")) s.WindowPadding = ImVec2(j["WindowPadding"][0], j["WindowPadding"][1]);
        if (j.contains("WindowRounding")) s.WindowRounding = j["WindowRounding"];
        if (j.contains("WindowBorderSize")) s.WindowBorderSize = j["WindowBorderSize"];
        if (j.contains("WindowMinSize")) s.WindowMinSize = ImVec2(j["WindowMinSize"][0], j["WindowMinSize"][1]);
        if (j.contains("WindowTitleAlign"))
            s.WindowTitleAlign = ImVec2(j["WindowTitleAlign"][0], j["WindowTitleAlign"][1]);
        if (j.contains("ChildRounding")) s.ChildRounding = j["ChildRounding"];
        if (j.contains("ChildBorderSize")) s.ChildBorderSize = j["ChildBorderSize"];
        if (j.contains("PopupRounding")) s.PopupRounding = j["PopupRounding"];
        if (j.contains("PopupBorderSize")) s.PopupBorderSize = j["PopupBorderSize"];
        if (j.contains("FramePadding")) s.FramePadding = ImVec2(j["FramePadding"][0], j["FramePadding"][1]);
        if (j.contains("FrameRounding")) s.FrameRounding = j["FrameRounding"];
        if (j.contains("FrameBorderSize")) s.FrameBorderSize = j["FrameBorderSize"];
        if (j.contains("ItemSpacing")) s.ItemSpacing = ImVec2(j["ItemSpacing"][0], j["ItemSpacing"][1]);
        if (j.contains("ItemInnerSpacing"))
            s.ItemInnerSpacing = ImVec2(j["ItemInnerSpacing"][0], j["ItemInnerSpacing"][1]);
        if (j.contains("CellPadding")) s.CellPadding = ImVec2(j["CellPadding"][0], j["CellPadding"][1]);
        if (j.contains("TouchExtraPadding"))
            s.TouchExtraPadding = ImVec2(j["TouchExtraPadding"][0], j["TouchExtraPadding"][1]);
        if (j.contains("IndentSpacing")) s.IndentSpacing = j["IndentSpacing"];
        if (j.contains("ColumnsMinSpacing")) s.ColumnsMinSpacing = j["ColumnsMinSpacing"];
        if (j.contains("ScrollbarSize")) s.ScrollbarSize = j["ScrollbarSize"];
        if (j.contains("ScrollbarRounding")) s.ScrollbarRounding = j["ScrollbarRounding"];
        if (j.contains("GrabMinSize")) s.GrabMinSize = j["GrabMinSize"];
        if (j.contains("GrabRounding")) s.GrabRounding = j["GrabRounding"];
        if (j.contains("LogSliderDeadzone")) s.LogSliderDeadzone = j["LogSliderDeadzone"];
        if (j.contains("TabRounding")) s.TabRounding = j["TabRounding"];
        if (j.contains("TabBorderSize")) s.TabBorderSize = j["TabBorderSize"];
        if (j.contains("TabMinWidthForCloseButton")) s.TabMinWidthForCloseButton = j["TabMinWidthForCloseButton"];
        if (j.contains("TabBarBorderSize")) s.TabBarBorderSize = j["TabBarBorderSize"];
        if (j.contains("TableAngledHeadersAngle")) s.TableAngledHeadersAngle = j["TableAngledHeadersAngle"];
        if (j.contains("TableAngledHeadersTextAlign"))
            s.TableAngledHeadersTextAlign =
                ImVec2(j["TableAngledHeadersTextAlign"][0], j["TableAngledHeadersTextAlign"][1]);
        if (j.contains("ButtonTextAlign"))
            s.ButtonTextAlign = ImVec2(j["ButtonTextAlign"][0], j["ButtonTextAlign"][1]);
        if (j.contains("SelectableTextAlign"))
            s.SelectableTextAlign = ImVec2(j["SelectableTextAlign"][0], j["SelectableTextAlign"][1]);
        if (j.contains("SeparatorTextBorderSize")) s.SeparatorTextBorderSize = j["SeparatorTextBorderSize"];
        if (j.contains("SeparatorTextAlign"))
            s.SeparatorTextAlign = ImVec2(j["SeparatorTextAlign"][0], j["SeparatorTextAlign"][1]);
        if (j.contains("SeparatorTextPadding"))
            s.SeparatorTextPadding = ImVec2(j["SeparatorTextPadding"][0], j["SeparatorTextPadding"][1]);
        if (j.contains("DisplayWindowPadding"))
            s.DisplayWindowPadding = ImVec2(j["DisplayWindowPadding"][0], j["DisplayWindowPadding"][1]);
        if (j.contains("DisplaySafeAreaPadding"))
            s.DisplaySafeAreaPadding = ImVec2(j["DisplaySafeAreaPadding"][0], j["DisplaySafeAreaPadding"][1]);
        if (j.contains("DockingSeparatorSize")) s.DockingSeparatorSize = j["DockingSeparatorSize"];
        if (j.contains("MouseCursorScale")) s.MouseCursorScale = j["MouseCursorScale"];
        if (j.contains("AntiAliasedLines")) s.AntiAliasedLines = j["AntiAliasedLines"];
        if (j.contains("AntiAliasedLinesUseTex")) s.AntiAliasedLinesUseTex = j["AntiAliasedLinesUseTex"];
        if (j.contains("AntiAliasedFill")) s.AntiAliasedFill = j["AntiAliasedFill"];
        if (j.contains("CurveTessellationTol")) s.CurveTessellationTol = j["CurveTessellationTol"];
        if (j.contains("CircleTessellationMaxError")) s.CircleTessellationMaxError = j["CircleTessellationMaxError"];

        // Shape values (rounding/border) are easy to get "silently" wrong in a theme
        // file (a typo'd key, a value nested under the wrong object) with no visual
        // cue as obvious as a missing color — log exactly what ended up in the live
        // ImGuiStyle so a theme author can confirm the numbers actually took.
        logger::info(
            "[Theme] '{}' shape: WindowRounding={} FrameRounding={} ChildRounding={} PopupRounding={} "
            "GrabRounding={} ScrollbarRounding={} TabRounding={} | WindowBorderSize={} FrameBorderSize={} "
            "PopupBorderSize={} ChildBorderSize={} TabBorderSize={}",
            path, s.WindowRounding, s.FrameRounding, s.ChildRounding, s.PopupRounding, s.GrabRounding,
            s.ScrollbarRounding, s.TabRounding, s.WindowBorderSize, s.FrameBorderSize, s.PopupBorderSize,
            s.ChildBorderSize, s.TabBorderSize);

        if (!j.contains("ImGuiCol")) {
            logger::info("[Theme] Applied '{}' ({}) — no ImGuiCol overrides, colors are StyleColorsDark defaults.",
                        path, fullPath);
            return;
        }

        auto& cols = j["ImGuiCol"];
        ImVec4* c = s.Colors;
        int applied = 0;
        int unrecognized = 0;

        for (auto& e : cols.items()) {
            const std::string& k = e.key();
            const std::string& v = e.value();

            bool matched = false;
            for (int i = 0; i < ImGuiCol_COUNT; i++) {
                if (std::string(ImGui::GetStyleColorName(i)) == k) {
                    c[i] = HexToImVec4(v);
                    matched = true;
                    ++applied;
                    break;
                }
            }
            if (!matched) {
                ++unrecognized;
                logger::warn("[Theme] '{}': unrecognized ImGuiCol key '{}' — check spelling against "
                            "fallout4.json (silently ignored, not applied).", path, k);
            }
        }

        logger::info("[Theme] Applied '{}' ({}) — {} color(s) set, {} key(s) unrecognized.", path, fullPath,
                    applied, unrecognized);
    } catch (const std::exception& e) {
        logger::warn("[Theme] Failed to apply theme '{}': {} — falling back to defaults for the rest of it.",
                     path, e.what());
    }
}

// --- Live reload -----------------------------------------------------------
// Snapshot of every .json file in the Themes folder (filename -> last-write
// time). Comparing this against the previous poll catches both "a theme
// file was added/removed" (refresh the dropdown) and "the active theme's
// JSON was edited in place" (reapply it) with a single check.
static std::unordered_map<std::string, std::filesystem::file_time_type> s_themeSnapshot;
static std::chrono::steady_clock::time_point s_lastThemePoll{};
static bool s_themeSnapshotPrimed = false;

bool Theme::PollForChanges() {
    // Throttled to once a second — this is called every frame the Settings
    // window is open, and a full directory scan every frame would be wasteful.
    const auto now = std::chrono::steady_clock::now();
    if (s_themeSnapshotPrimed && now - s_lastThemePoll < std::chrono::seconds(1)) {
        return false;
    }
    s_lastThemePoll = now;

    std::unordered_map<std::string, std::filesystem::file_time_type> current;
    if (std::filesystem::exists(kThemesFolder)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(kThemesFolder)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".json") continue;
                std::error_code ec;
                const auto mtime = entry.last_write_time(ec);
                if (ec) continue;
                current[ThemePathUtf8(entry.path().filename())] = mtime;
            }
        } catch (const std::system_error& e) {
            logger::warn("[Theme] Live-reload scan aborted: {}", e.what());
        }
    }

    // The very first poll just primes the snapshot — there is nothing to
    // "reload" relative to, since nothing has been observed yet.
    const bool wasPrimed = s_themeSnapshotPrimed;
    s_themeSnapshotPrimed = true;
    const bool changed = wasPrimed && (current != s_themeSnapshot);
    s_themeSnapshot = std::move(current);
    return changed;
}
