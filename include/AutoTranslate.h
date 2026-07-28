#pragma once

// Automatic backend translation of third-party plugin UI text.
//
// Plugins render through this DLL's cimgui exports (ig*), so every
// player-facing string they draw crosses our boundary. This module
// substitutes translated strings transparently: the framework resolves
// which plugin's render callback is currently executing (by the callback
// address's module), loads that plugin's JSON tables via PluginLocalization
// (Data/F4SE/Plugins/<Name>/Languages/<lang>.json), and swaps labels and
// format strings on the way into ImGui. Plugin authors do not need to call
// any API; translators can localize any framework plugin retroactively.
//
// Crash-safety contract (every entry point):
//   - Any failure, miss, or doubt returns the caller's pointer unchanged.
//   - Format strings are substituted ONLY when the translation consumes
//     printf varargs identically to the original
//     (PluginLocalization::FormatSpecsCompatible); a mismatched
//     translation would make vsnprintf read garbage off the stack.
//   - Lookup misses are never interned, so arbitrary per-frame dynamic
//     strings cannot grow memory.
//   - No exceptions escape; everything is try/caught to passthrough.
namespace AutoTranslate {

    // Master switch ([Localization] AutoTranslate in the INI).
    void SetEnabled(bool on);
    bool IsEnabled();

    // Capture mode ([Localization] CaptureStrings): records every unique
    // string plugins draw and writes per-plugin skeleton JSONs on menu
    // close, giving translators a ready-to-fill string list without source
    // access.
    void SetCaptureEnabled(bool on);
    bool IsCaptureEnabled();

    // Records the path a plugin passed to AddSectionItem
    // ("Mod Name/Page"). The first segment becomes the module's
    // translation folder name (falls back to the DLL file name stem for
    // modules that only add windows or HUD elements).
    void NoteSectionPath(void* renderFn, const char* fullPath);

    // RAII plugin context wrapped around every third-party render callback
    // invocation (section pages, plugin windows, HUD elements). While a
    // scope is active on this thread, the lookup functions below translate
    // against that plugin's tables; with no scope they pass through.
    class Scope {
    public:
        explicit Scope(void* callbackAddress);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        void* previous_;
    };

    // ---- Lookup entry points for the cimgui export layer ----

    // Plain widget label (never printf-processed by ImGui). Handles
    // "Visible##id" by translating the visible part and preserving the ID
    // suffix so widget identity survives.
    const char* Label(const char* label);

    // printf format string (Text/TextWrapped/SetTooltip/TreeNode...).
    // Substitutes only specifier-compatible translations.
    const char* Format(const char* fmt);

    // (text, text_end) pair: substituted only when textEnd is null
    // (a substring view cannot be looked up or safely replaced).
    const char* Range(const char* text, const char* textEnd);

    // Item arrays for Combo/ListBox. Returns a translated pointer array
    // valid for the duration of the current call, or `items` unchanged.
    const char* const* LabelArray(const char* const items[], int count);

    // "A\0B\0\0" item blobs for Combo. Returns a translated blob (interned,
    // stable) or `itemsBlob` unchanged.
    const char* ZeroSeparated(const char* itemsBlob);

    // Nav-tree page titles: rendered by the framework on behalf of a
    // plugin, so the plugin is identified by the page's render callback
    // instead of the active scope.
    const char* NodeTitle(void* renderFn, const char* title);

    // Writes captured string skeletons to
    // Data/F4SE/Plugins/<Name>/Languages/captured_strings.json.
    // Called when the framework menu closes.
    void FlushCapture();

}
