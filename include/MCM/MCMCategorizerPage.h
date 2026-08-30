#pragma once

#include <string>
#include <vector>

// Native recreation of m8r98a4f2's "MCM Categorizer" (Nexus 66311), ported
// from the decompiled AS3 under PluginTemplate/MCM Categorizer/_analysis/:
//   Service/CategoryService.as  -> category model + mutations
//   Service/OrderService.as     -> order vector semantics (MCM's sOrder:Main)
//   Model/McmCategorizerSettings.as -> style settings
//   Helper/FastObjectStringifier.as -> wire codec (see MCM/M8rQckSer.h)
//
// Two halves:
//   1. GROUPING — folds the framework's translated MCM mod list into the
//      user's categories. MCMWidgetRenderer consults CategoryLabelFor /
//      OrderIndexFor while building the "MCM Mod Configs (Legacy)" section
//      tree, so categorized mods nest under their category folder in the
//      user-defined order.
//   2. EDITOR — the categorizer's own MCM page embeds its Flash editor app
//      in an "image" control; HandlesImageControl/RenderImageControl replace
//      it with a native ImGui editor (add/rename/delete categories, assign
//      and reorder mods, sort), exactly like the MCM Settings Manager and
//      FallUI recreations.
//
// STANDALONE REPLACEMENT (does not require the m8r mod to be installed):
// the model is owned by the framework and persisted to a PRIVATE store,
//   Data/MCM/Settings/F4SEMenuFrameworkCategories.ini
// which the MCM Settings Manager never enumerates (it is not a registered
// MCM config). This is the crash fix: the m8r categorizer round-tripped its
// category data through MCM's own "MCM/sOrder:Main" setting, and the
// abandoned Flash Settings Manager recursed itself into a stack overflow
// serializing that on save. We never write MCM/sOrder:Main anymore.
//
// FILE COMPATIBILITY / MIGRATION: a user's existing setup carries over. On
// first load we import the legacy m8r values (sCategories, MCM/sOrder:Main,
// wrap/enable style) into the private store. When the m8r categorizer mod is
// gone, we also flatten the leftover categorized MCM/sOrder:Main (strip the
// "__CAT_<id>" markers) so the removed-but-lingering value can't re-trigger
// the Settings Manager crash. The legacy m8rQckSer category array format is
// preserved in our store, so the data stays inspectable and reversible.
namespace MCMCategorizerPage {

    // One scanned MCM mod, supplied by MCMWidgetRenderer before the section
    // tree is built. `folder` is the Data/MCM/Config/<folder> name (used in
    // sOrder), `configModName` is config.json's "modName" (used in category
    // membership lists), `display` is the translated display name.
    struct ModCatalogEntry {
        std::string folder;
        std::string configModName;
        std::string display;
    };
    void SetCatalog(std::vector<ModCatalogEntry> mods);

    // True when categorization is enabled (bEnabled), the category data
    // parsed cleanly, and at least one category exists — grouping only
    // applies then. No longer depends on the m8r mod being installed.
    bool CategorizationActive();

    // Nav-tree folder label for the category containing this config modName
    // (already wrapped per bFolderWrap/sWrapBefore/sWrapAfter and sanitized
    // for path use). Empty string = not categorized.
    std::string CategoryLabelFor(const std::string& configModName);

    // Global position of a mod folder in the normalized order (category
    // members count at their category's position). INT_MAX when unlisted,
    // so unknown mods keep their registration order at the end.
    int OrderIndexFor(const std::string& folderName);

    // True when this nav-tree node name is one of the active category
    // folders (exact match against the labels CategoryLabelFor produces).
    // The tree renderer uses it to style categories distinctly (folder
    // icon, accent color, larger text) from ordinary mods-with-pages.
    bool IsCategoryNavLabel(const std::string& nodeName);

    // True when this MCM image control is the m8r MCM Categorizer editor app.
    // Only matches while that mod is still installed (transitional); the
    // standalone editor is reached through the framework's own nav entry.
    bool HandlesImageControl(const std::string& libName, const std::string& className);

    // Renders the native editor inside the m8r mod's hijacked image control.
    // Call only when HandlesImageControl() is true.
    void RenderImageControl(const std::string& libName, const std::string& className);

    // Renders the native category editor standalone. This is the framework's
    // own entry point (a synthetic nav item registered by MCMWidgetRenderer),
    // so the editor is reachable even with the m8r categorizer mod removed.
    void RenderEditor();

    // Nav label of the standalone editor entry. Shared so the renderer that
    // registers it and the tool-accent predicate agree on one string.
    inline constexpr const char* kEditorNavLabel = "Edit Categories";

    // True while the original m8r MCM Categorizer mod is installed (its
    // config.json is present). Drives editor-entry visibility and top pinning.
    bool IsLegacyModInstalled();

    // True for the nav labels that should read as "categorizer tools" pinned
    // at the top of the legacy list — the m8r "MCM Categorizer" page and our
    // "Edit Categories" entry. The tree renderer accents these leaf nodes.
    bool IsCategorizerToolNavLabel(const std::string& nodeName);

    // Drops cached data so the next use reloads from the value provider.
    // Called on menu close / page leave / MCM.RefreshMenu.
    void ResetSession();

    // Re-checks the nav-affecting data (bEnabled, wrap chars, categories,
    // order) against what the current section tree was built from. Returns
    // true when they differ — the caller then queues a tree rebuild. Used
    // when the user leaves the categorizer's page (style switchers live
    // there as plain translated MCM controls we don't otherwise observe).
    bool NavDataChanged();

}
