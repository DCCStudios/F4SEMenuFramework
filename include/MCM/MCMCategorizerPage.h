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
// DATA-FORMAT COMPATIBILITY IS THE CONTRACT: everything is persisted to the
// same three MCM settings the Flash app uses, so both UIs stay in sync:
//   MCMCategorizer/sCategories:MCMCategorizer   (m8rQckSer category array)
//   MCMCategorizer/sOrderModNames:MCMCategorizer (CSV, config modNames)
//   MCM/sOrder:Main                              (CSV, config FOLDER names +
//                                                 "__CAT_<id>" markers — the
//                                                 authoritative order)
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

    // True when MCM Categorizer is installed, enabled (bEnabled) and its
    // category data parsed cleanly — grouping only applies then.
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

    // True when this MCM image control is the MCM Categorizer editor app.
    bool HandlesImageControl(const std::string& libName, const std::string& className);

    // Renders the native editor. Call only when HandlesImageControl() is true.
    void RenderImageControl(const std::string& libName, const std::string& className);

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
