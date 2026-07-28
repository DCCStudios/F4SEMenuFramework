#include "MCM/MCMCategorizerPage.h"
#include "MCM/M8rQckSer.h"
#include "MCM/MCMConfigParser.h"
#include "MCM/MCMScanner.h"
#include "MCM/MCMTranslation.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMWidgetRenderer.h"
#include "imgui.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ============================================================================
// Native recreation of "MCM Categorizer" (Nexus 66311). See the header for
// the module overview and the compatibility contract.
//
// Model notes (from the decompiled AS3):
//  * sCategories is an ARRAY of { name, mods[], id, dirName:"__CAT_<id>" }
//    objects in m8rQckSer plain encoding. `mods` holds config.json modNames.
//  * The authoritative display order is MCM's OWN setting "sOrder:Main"
//    (mod "MCM"): a CSV of config FOLDER names interleaved with "__CAT_<id>"
//    markers. CategoryService.sortModsByCats normalizes it so a category's
//    members sit directly after their marker — we normalize the same way on
//    load. sOrderModNames is a modName-keyed mirror written alongside.
//  * Entries the framework didn't scan (the Flash app's virtual "MCM" /
//    "Hotkey manager" rows, or mods that are currently uninstalled) are
//    PRESERVED verbatim through edits and shown greyed out as "[name]",
//    matching OrderService.getModInfo's fallback.
// ============================================================================

namespace MCMCategorizerPage {

    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    static constexpr const char* kModFolder = "MCMCategorizer";
    static constexpr const char* kCategoriesKey = "sCategories:MCMCategorizer";
    static constexpr const char* kOrderNamesKey = "sOrderModNames:MCMCategorizer";
    static constexpr const char* kOrderMod = "MCM";
    static constexpr const char* kOrderKey = "sOrder:Main";
    static constexpr const char* kMarkerPrefix = "__CAT_";

    // ------------------------------------------------------------------
    // Translations (Data/MCM/Config/MCMCategorizer/Translation/)
    // ------------------------------------------------------------------

    static MCMTranslation::TranslationMap s_tr;
    static bool s_trLoaded = false;

    static std::string Tr(const std::string& key) {
        if (!s_trLoaded) {
            s_trLoaded = true;
            std::error_code ec;
            fs::path dir = MCMScanner::GetScanBasePath() / kModFolder / "Translation";
            if (fs::exists(dir, ec)) {
                s_tr = MCMTranslation::LoadDirectory(dir);
            }
        }
        return MCMTranslation::ResolveAndStrip(key, s_tr);
    }

    // ------------------------------------------------------------------
    // Session state
    // ------------------------------------------------------------------

    struct Category {
        int id = 0;
        std::string name;                   // raw display name (may contain '/')
        std::string marker;                 // "__CAT_<id>"
        // Ordered member keys: FOLDER name for scanned mods, the raw config
        // modName for unknown entries. Order = normalized display order.
        std::vector<std::string> members;
    };

    struct TopItem {
        bool isCategory = false;
        size_t cat = 0;        // index into categories when isCategory
        std::string modKey;    // member-key of a top-level mod otherwise
    };

    struct Session {
        bool loaded = false;
        bool installed = false;
        bool corrupt = false;   // sCategories present but unparseable — read-only mode

        // Nav labels of all active categories (what CategoryLabelFor emits),
        // for the tree renderer's category styling.
        std::set<std::string> navLabels;

        // Style settings (subset that affects our nav tree)
        bool enabled = true;
        bool folderWrap = true;
        std::string wrapBefore;
        std::string wrapAfter;

        std::vector<Category> categories;
        std::vector<TopItem> items;

        // memberKey -> category index (membership lookup during order walks)
        std::map<std::string, size_t> memberOf;
        // folder name -> global normalized order position
        std::map<std::string, int> orderIndex;

        // Raw strings the current state was loaded from (change detection
        // for NavDataChanged after the user toggles style switchers).
        std::string rawCategories, rawOrder, rawEnabled, rawWrap, rawBefore, rawAfter;
    };

    static Session s;

    // Catalog supplied by MCMWidgetRenderer (survives ResetSession — it only
    // changes when mods are re-registered, i.e. never mid-game).
    static std::vector<ModCatalogEntry> s_catalog;
    static std::map<std::string, const ModCatalogEntry*> s_byFolder;
    static std::map<std::string, const ModCatalogEntry*> s_byConfigName;

    void SetCatalog(std::vector<ModCatalogEntry> mods) {
        s_catalog = std::move(mods);
        s_byFolder.clear();
        s_byConfigName.clear();
        for (const auto& e : s_catalog) {
            s_byFolder.emplace(e.folder, &e);
            // First registration wins on duplicate config modNames.
            s_byConfigName.emplace(e.configModName, &e);
        }
        s.loaded = false;  // mappings feed the model — rebuild it
    }

    void ResetSession() {
        s = Session{};
    }

    // ------------------------------------------------------------------
    // Raw setting access
    // ------------------------------------------------------------------

    static std::string GetRaw(const std::string& mod, const std::string& key) {
        auto v = MCMValueProvider::GetModSettingRaw(mod, key);
        return v.value_or("");
    }

    // Writes a string mod-setting through the TYPED provider path so the
    // value also mirrors into the real MCM's live in-memory store when it is
    // present (the Flash categorizer reads these settings from that store,
    // not from disk). GetModSettingRaw/SetModSettingRaw skip the mirror.
    static void SetStringSetting(const std::string& mod, const std::string& key,
                                 const std::string& value) {
        MCMConfigParser::ValueSource src;
        src.type = MCMConfigParser::SourceType::ModSettingString;
        src.settingName = key;
        MCMValueProvider::SetString(mod, src, value);
    }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    static bool IsMarker(const std::string& v) {
        return v.rfind(kMarkerPrefix, 0) == 0;
    }

    // Small UTF-8 validity check — the wrap characters ship as « » and some
    // installs carry them in ANSI encoding (a lone 0xAB/0xBB byte), which
    // ImGui would render as garbage. Fall back to UTF-8 guillemets then.
    static bool IsValidUtf8(const std::string& v) {
        size_t i = 0;
        while (i < v.size()) {
            const unsigned char c = static_cast<unsigned char>(v[i]);
            size_t extra;
            if (c < 0x80) extra = 0;
            else if ((c >> 5) == 0x6) extra = 1;
            else if ((c >> 4) == 0xE) extra = 2;
            else if ((c >> 3) == 0x1E) extra = 3;
            else return false;
            if (i + extra >= v.size()) return false;
            for (size_t k = 1; k <= extra; ++k) {
                if ((static_cast<unsigned char>(v[i + k]) & 0xC0) != 0x80) return false;
            }
            i += extra + 1;
        }
        return true;
    }

    static const ModCatalogEntry* EntryForKey(const std::string& key) {
        auto it = s_byFolder.find(key);
        return it != s_byFolder.end() ? it->second : nullptr;
    }

    // Display label of a member key ("[name]" for unknown entries, AS3 style).
    static std::string DisplayForKey(const std::string& key) {
        if (const auto* e = EntryForKey(key)) return e->display;
        return "[" + key + "]";
    }

    static std::string ConfigNameForKey(const std::string& key) {
        if (const auto* e = EntryForKey(key)) return e->configModName;
        return key;
    }

    // Nav-tree folder label for a category: the wrapped display name, with
    // '/' swapped for '-' because '/' nests in the framework's path-based
    // tree ("Controls/Game Settings" must stay ONE folder) and fancy slash
    // lookalikes are outside the game-style fonts' glyph coverage.
    static std::string NavLabelForCategory(const Category& c) {
        std::string label = s.folderWrap ? s.wrapBefore + c.name + s.wrapAfter : c.name;
        if (label.empty()) label = "Category " + std::to_string(c.id);
        for (char& ch : label) {
            if (ch == '/') ch = '-';
        }
        return label;
    }

    // ------------------------------------------------------------------
    // Load + normalization (port of loadCategories/loadOrder/sortModsByCats)
    // ------------------------------------------------------------------

    static void RebuildIndexes() {
        s.memberOf.clear();
        for (size_t c = 0; c < s.categories.size(); ++c) {
            for (const auto& key : s.categories[c].members) {
                s.memberOf.emplace(key, c);
            }
        }
        s.orderIndex.clear();
        int idx = 0;
        for (const auto& item : s.items) {
            if (item.isCategory) {
                for (const auto& key : s.categories[item.cat].members) {
                    s.orderIndex.emplace(key, idx++);
                }
            } else {
                s.orderIndex.emplace(item.modKey, idx++);
            }
        }
    }

    static void EnsureLoaded() {
        if (s.loaded) return;
        s.loaded = true;

        std::error_code ec;
        s.installed = fs::exists(MCMScanner::GetScanBasePath() / kModFolder / "config.json", ec);
        if (!s.installed) return;

        // Style settings (bEnabled default is 1 per the shipped settings.ini,
        // which GetModSettingRaw layers underneath the user file).
        s.rawEnabled = GetRaw(kModFolder, "bEnabled:MCMCategorizer");
        s.rawWrap = GetRaw(kModFolder, "bFolderWrap:MCMCategorizer");
        s.rawBefore = GetRaw(kModFolder, "sWrapBefore:MCMCategorizer");
        s.rawAfter = GetRaw(kModFolder, "sWrapAfter:MCMCategorizer");
        s.enabled = s.rawEnabled.empty() || s.rawEnabled == "1" || s.rawEnabled == "true";
        s.folderWrap = s.rawWrap.empty() || s.rawWrap == "1" || s.rawWrap == "true";
        s.wrapBefore = s.rawBefore;
        s.wrapAfter = s.rawAfter;
        if (!IsValidUtf8(s.wrapBefore)) s.wrapBefore = "\xC2\xAB";  // «
        if (!IsValidUtf8(s.wrapAfter)) s.wrapAfter = "\xC2\xBB";    // »

        // Categories
        s.rawCategories = GetRaw(kModFolder, kCategoriesKey);
        s.categories.clear();
        if (!s.rawCategories.empty()) {
            auto parsed = M8rQckSer::Parse(s.rawCategories);
            if (!parsed.has_value() || !parsed->IsArray()) {
                s.corrupt = true;
                logger::warn("[MCMCategorizer] sCategories did not parse — categorization "
                             "disabled, editor read-only (value length {})",
                             s.rawCategories.size());
            } else {
                for (const auto& catVal : parsed->array) {
                    const auto* name = catVal.Find("name");
                    const auto* dir = catVal.Find("dirName");
                    const auto* mods = catVal.Find("mods");
                    const auto* id = catVal.Find("id");
                    if (!catVal.IsObject() || !name || !name->IsString() ||
                        !dir || !dir->IsString() || !IsMarker(dir->strVal) ||
                        !mods || !mods->IsArray()) {
                        logger::warn("[MCMCategorizer] Skipping malformed category entry");
                        continue;
                    }
                    Category c;
                    c.name = name->strVal;
                    c.marker = dir->strVal;
                    c.id = id && id->IsNumber() ? static_cast<int>(id->numVal)
                                                : std::atoi(c.marker.c_str() + std::strlen(kMarkerPrefix));
                    for (const auto& m : mods->array) {
                        if (!m.IsString()) continue;
                        // Membership is stored as config modNames; resolve to
                        // our folder-based member key when the mod is scanned.
                        const auto it = s_byConfigName.find(m.strVal);
                        c.members.push_back(it != s_byConfigName.end() ? it->second->folder
                                                                       : m.strVal);
                    }
                    s.categories.push_back(std::move(c));
                }
            }
        }

        // Membership lookup for the order walk (also dedupes: first category
        // claiming a mod wins, like the AS3's linear search).
        std::map<std::string, size_t> memberOf;
        for (size_t c = 0; c < s.categories.size(); ++c) {
            for (const auto& key : s.categories[c].members) {
                memberOf.emplace(key, c);
            }
        }

        // Order (authoritative: MCM's sOrder:Main — folder names + markers).
        s.rawOrder = GetRaw(kOrderMod, kOrderKey);
        std::vector<std::string> orderVec;
        {
            size_t start = 0;
            while (start <= s.rawOrder.size() && !s.rawOrder.empty()) {
                size_t comma = s.rawOrder.find(',', start);
                std::string tok = comma == std::string::npos
                                      ? s.rawOrder.substr(start)
                                      : s.rawOrder.substr(start, comma - start);
                if (!tok.empty()) orderVec.push_back(std::move(tok));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }

        // Normalized model: walk the order, folding members into their
        // category (their relative order is preserved — findAndSortCatMods
        // semantics), everything else becomes a top-level item.
        s.items.clear();
        std::set<std::string> seenTop;                       // top-level keys emitted
        std::set<size_t> seenCats;                           // categories emitted
        std::vector<std::vector<std::string>> orderedMembers(s.categories.size());
        std::vector<std::set<std::string>> memberSeen(s.categories.size());

        for (const auto& entry : orderVec) {
            if (IsMarker(entry)) {
                for (size_t c = 0; c < s.categories.size(); ++c) {
                    if (s.categories[c].marker == entry && !seenCats.count(c)) {
                        seenCats.insert(c);
                        s.items.push_back(TopItem{ true, c, {} });
                        break;
                    }
                }
                continue;  // stale markers (deleted categories) are dropped
            }
            auto mIt = memberOf.find(entry);
            if (mIt != memberOf.end()) {
                if (memberSeen[mIt->second].insert(entry).second) {
                    orderedMembers[mIt->second].push_back(entry);
                }
            } else if (seenTop.insert(entry).second) {
                s.items.push_back(TopItem{ false, 0, entry });
            }
        }

        // Categories that never appeared in the order go to the end.
        for (size_t c = 0; c < s.categories.size(); ++c) {
            if (!seenCats.count(c)) {
                s.items.push_back(TopItem{ true, c, {} });
            }
        }
        // Members listed in a category but missing from the order keep their
        // membership-array order at the end of the category.
        for (size_t c = 0; c < s.categories.size(); ++c) {
            for (const auto& key : s.categories[c].members) {
                if (memberSeen[c].insert(key).second) {
                    orderedMembers[c].push_back(key);
                }
            }
            s.categories[c].members = std::move(orderedMembers[c]);
        }
        // Scanned mods nobody mentioned append at the end (AS3 index 1000+).
        for (const auto& e : s_catalog) {
            if (!memberOf.count(e.folder) && !seenTop.count(e.folder)) {
                seenTop.insert(e.folder);
                s.items.push_back(TopItem{ false, 0, e.folder });
            }
        }

        RebuildIndexes();

        s.navLabels.clear();
        if (s.enabled && !s.corrupt) {
            for (const auto& c : s.categories) {
                s.navLabels.insert(NavLabelForCategory(c));
            }
        }
    }

    // ------------------------------------------------------------------
    // Persistence (port of saveCategories + writeOrder)
    // ------------------------------------------------------------------

    static void Save() {
        if (s.corrupt) return;  // never overwrite data we could not read

        // sCategories — category array in m8rQckSer plain encoding, key order
        // matching CategoryService.addCategory ({name, mods, id, dirName}).
        M8rQckSer::Value root = M8rQckSer::Value::MakeArray();
        for (const auto& c : s.categories) {
            M8rQckSer::Value cat = M8rQckSer::Value::MakeObject();
            cat.object.emplace_back("name", M8rQckSer::Value::MakeString(c.name));
            M8rQckSer::Value mods = M8rQckSer::Value::MakeArray();
            for (const auto& key : c.members) {
                mods.array.push_back(M8rQckSer::Value::MakeString(ConfigNameForKey(key)));
            }
            cat.object.emplace_back("mods", std::move(mods));
            cat.object.emplace_back("id", M8rQckSer::Value::MakeNumber(c.id));
            cat.object.emplace_back("dirName", M8rQckSer::Value::MakeString(c.marker));
            root.array.push_back(std::move(cat));
        }
        SetStringSetting(kModFolder, kCategoriesKey, M8rQckSer::Stringify(root));

        // Order — expanded normalized sequence. sOrder:Main gets folder
        // names; sOrderModNames mirrors it with config modNames (writeOrder).
        std::string orderDirs, orderNames;
        auto append = [](std::string& csv, const std::string& v) {
            if (!csv.empty()) csv.push_back(',');
            csv += v;
        };
        for (const auto& item : s.items) {
            if (item.isCategory) {
                const Category& c = s.categories[item.cat];
                append(orderDirs, c.marker);
                append(orderNames, c.marker);
                for (const auto& key : c.members) {
                    append(orderDirs, key);
                    append(orderNames, ConfigNameForKey(key));
                }
            } else {
                append(orderDirs, item.modKey);
                append(orderNames, ConfigNameForKey(item.modKey));
            }
        }
        SetStringSetting(kOrderMod, kOrderKey, orderDirs);
        SetStringSetting(kModFolder, kOrderNamesKey, orderNames);
        MCMValueProvider::FlushAll();

        logger::info("[MCMCategorizer] Saved {} categories, {} order entries",
                     s.categories.size(), s.items.size());

        // The nav tree must regroup. Reload from what we just wrote so the
        // editor state and the snapshot strings stay consistent.
        ResetSession();
        MCMWidgetRenderer::QueueSectionTreeRebuild();
    }

    // ------------------------------------------------------------------
    // Grouping API (used while building the section tree)
    // ------------------------------------------------------------------

    bool CategorizationActive() {
        EnsureLoaded();
        return s.installed && s.enabled && !s.corrupt && !s.categories.empty();
    }

    std::string CategoryLabelFor(const std::string& configModName) {
        EnsureLoaded();
        if (!s.installed || !s.enabled || s.corrupt) return "";
        const auto it = s_byConfigName.find(configModName);
        const std::string key = it != s_byConfigName.end() ? it->second->folder : configModName;
        const auto mIt = s.memberOf.find(key);
        if (mIt == s.memberOf.end()) return "";

        return NavLabelForCategory(s.categories[mIt->second]);
    }

    int OrderIndexFor(const std::string& folderName) {
        EnsureLoaded();
        if (!s.installed || !s.enabled || s.corrupt) return INT_MAX;
        const auto it = s.orderIndex.find(folderName);
        return it != s.orderIndex.end() ? it->second : INT_MAX;
    }

    bool IsCategoryNavLabel(const std::string& nodeName) {
        if (!s.loaded) {
            // Queried every frame per tree node; only (re)load when the MCM
            // layer actually registered mods (SetCatalog ran).
            if (s_catalog.empty()) return false;
            EnsureLoaded();
        }
        return s.navLabels.count(nodeName) != 0;
    }

    bool NavDataChanged() {
        if (!s.loaded || !s.installed) return false;
        const bool changed =
            GetRaw(kModFolder, "bEnabled:MCMCategorizer") != s.rawEnabled ||
            GetRaw(kModFolder, "bFolderWrap:MCMCategorizer") != s.rawWrap ||
            GetRaw(kModFolder, "sWrapBefore:MCMCategorizer") != s.rawBefore ||
            GetRaw(kModFolder, "sWrapAfter:MCMCategorizer") != s.rawAfter ||
            GetRaw(kModFolder, kCategoriesKey) != s.rawCategories ||
            GetRaw(kOrderMod, kOrderKey) != s.rawOrder;
        if (changed) {
            ResetSession();  // rebuilt from fresh data by the queued rebuild
        }
        return changed;
    }

    // ------------------------------------------------------------------
    // Editor (replaces the Flash McmPlugin app in the mod's image control)
    // ------------------------------------------------------------------

    bool HandlesImageControl(const std::string& libName, const std::string& className) {
        return libName == "MCMCategorizer" &&
               className == "M8r.McmCategorizer.Controller.McmPlugin";
    }

    // Identifies a draggable/droppable row for the mouse drag-and-drop path
    // (POD, memcpy'd through the ImGui payload):
    //   kind 0 = top-level mod row      a = top-item index
    //   kind 1 = category member row    a = category index, b = member index
    //   kind 2 = category header        a = category index, b = top-item index
    struct DragRef {
        int kind = -1;
        int a = 0, b = 0;
    };
    static constexpr const char* kDragPayload = "MCMCAT_DND";

    // Deferred mutation — applied after the draw loop so we never mutate the
    // model while iterating it.
    struct PendingAction {
        enum class Kind {
            None, AddCategory, RenameCategory, DeleteCategory, SortCategory,
            MoveItem, MoveMember, RemoveMember, AssignMember, SortAll, DragDrop
        };
        Kind kind = Kind::None;
        size_t a = 0, b = 0;   // indexes (item/category/member as needed)
        int dir = 0;           // -1 up / +1 down
        std::string text;      // new name for Add/Rename
        DragRef src, dst;      // DragDrop only
    };

    // Editor UI state (survives ResetSession on purpose — popups stay open
    // across the model reload a save triggers).
    static char s_nameBuf[128] = {};
    static size_t s_targetCat = 0;
    static bool s_openAdd = false, s_openRename = false, s_openDelete = false, s_openSortAll = false;

    static int NextCatId() {
        int next = 1;
        for (const auto& c : s.categories) next = std::max(next, c.id + 1);
        return next;
    }

    static void Apply(const PendingAction& act) {
        using K = PendingAction::Kind;
        switch (act.kind) {
            case K::AddCategory: {
                Category c;
                c.id = NextCatId();
                c.name = act.text;
                c.marker = kMarkerPrefix + std::to_string(c.id);
                s.categories.push_back(std::move(c));
                s.items.push_back(TopItem{ true, s.categories.size() - 1, {} });
                break;
            }
            case K::RenameCategory:
                if (act.a < s.categories.size()) s.categories[act.a].name = act.text;
                break;
            case K::DeleteCategory: {
                if (act.a >= s.categories.size()) break;
                // Members return to the main level at the category's position
                // (the AS3 removes the marker; the member order entries stay).
                for (size_t i = 0; i < s.items.size(); ++i) {
                    if (s.items[i].isCategory && s.items[i].cat == act.a) {
                        std::vector<TopItem> freed;
                        for (const auto& key : s.categories[act.a].members) {
                            freed.push_back(TopItem{ false, 0, key });
                        }
                        s.items.erase(s.items.begin() + static_cast<ptrdiff_t>(i));
                        s.items.insert(s.items.begin() + static_cast<ptrdiff_t>(i),
                                       freed.begin(), freed.end());
                        break;
                    }
                }
                s.categories.erase(s.categories.begin() + static_cast<ptrdiff_t>(act.a));
                // Category indexes above the removed one shift down.
                for (auto& item : s.items) {
                    if (item.isCategory && item.cat > act.a) --item.cat;
                }
                break;
            }
            case K::SortCategory:
                if (act.a < s.categories.size()) {
                    auto& m = s.categories[act.a].members;
                    std::sort(m.begin(), m.end(), [](const std::string& x, const std::string& y) {
                        return _stricmp(DisplayForKey(x).c_str(), DisplayForKey(y).c_str()) < 0;
                    });
                }
                break;
            case K::MoveItem: {
                const size_t j = act.a + static_cast<size_t>(act.dir);
                if (act.a < s.items.size() && j < s.items.size()) {
                    std::swap(s.items[act.a], s.items[j]);
                }
                break;
            }
            case K::MoveMember: {
                if (act.a >= s.categories.size()) break;
                auto& m = s.categories[act.a].members;
                const size_t j = act.b + static_cast<size_t>(act.dir);
                if (act.b < m.size() && j < m.size()) {
                    std::swap(m[act.b], m[j]);
                }
                break;
            }
            case K::RemoveMember: {
                if (act.a >= s.categories.size()) break;
                auto& m = s.categories[act.a].members;
                if (act.b >= m.size()) break;
                std::string key = m[act.b];
                m.erase(m.begin() + static_cast<ptrdiff_t>(act.b));
                // The mod reappears right after its former category.
                for (size_t i = 0; i < s.items.size(); ++i) {
                    if (s.items[i].isCategory && s.items[i].cat == act.a) {
                        s.items.insert(s.items.begin() + static_cast<ptrdiff_t>(i + 1),
                                       TopItem{ false, 0, std::move(key) });
                        break;
                    }
                }
                break;
            }
            case K::AssignMember: {
                if (act.b >= s.categories.size() || act.a >= s.items.size()) break;
                if (s.items[act.a].isCategory) break;
                std::string key = s.items[act.a].modKey;
                s.items.erase(s.items.begin() + static_cast<ptrdiff_t>(act.a));
                s.categories[act.b].members.push_back(std::move(key));
                break;
            }
            case K::SortAll: {
                for (auto& c : s.categories) {
                    std::sort(c.members.begin(), c.members.end(),
                              [](const std::string& x, const std::string& y) {
                        return _stricmp(DisplayForKey(x).c_str(), DisplayForKey(y).c_str()) < 0;
                    });
                }
                std::sort(s.items.begin(), s.items.end(),
                          [](const TopItem& x, const TopItem& y) {
                    const std::string lx = x.isCategory ? s.categories[x.cat].name
                                                        : DisplayForKey(x.modKey);
                    const std::string ly = y.isCategory ? s.categories[y.cat].name
                                                        : DisplayForKey(y.modKey);
                    return _stricmp(lx.c_str(), ly.c_str()) < 0;
                });
                break;
            }
            case K::DragDrop: {
                const DragRef& src = act.src;
                const DragRef& dst = act.dst;

                if (src.kind == 2) {
                    // Reorder a category among the top-level items. Only
                    // top-level rows are meaningful targets (dropping a
                    // category into another category is not a thing).
                    size_t from = static_cast<size_t>(src.b);
                    size_t to;
                    if (dst.kind == 0) to = static_cast<size_t>(dst.a);
                    else if (dst.kind == 2) to = static_cast<size_t>(dst.b);
                    else return;
                    if (from >= s.items.size() || to >= s.items.size() || from == to) return;
                    TopItem moved = s.items[from];
                    s.items.erase(s.items.begin() + static_cast<ptrdiff_t>(from));
                    if (from < to) --to;
                    s.items.insert(s.items.begin() + static_cast<ptrdiff_t>(to), moved);
                    break;
                }

                // Moving a mod row. Validate + read the key first, then
                // remove from source and insert at destination (indexes are
                // adjusted when source and destination share a container).
                std::string key;
                if (src.kind == 0) {
                    if (static_cast<size_t>(src.a) >= s.items.size() ||
                        s.items[src.a].isCategory) return;
                    key = s.items[src.a].modKey;
                } else if (src.kind == 1) {
                    if (static_cast<size_t>(src.a) >= s.categories.size() ||
                        static_cast<size_t>(src.b) >= s.categories[src.a].members.size()) return;
                    key = s.categories[src.a].members[src.b];
                } else {
                    return;
                }
                auto removeSrc = [&] {
                    if (src.kind == 0) {
                        s.items.erase(s.items.begin() + src.a);
                    } else {
                        auto& m = s.categories[src.a].members;
                        m.erase(m.begin() + src.b);
                    }
                };

                if (dst.kind == 2) {
                    // Dropped on a category header: append to that category.
                    if (static_cast<size_t>(dst.a) >= s.categories.size()) return;
                    removeSrc();
                    s.categories[dst.a].members.push_back(std::move(key));
                } else if (dst.kind == 1) {
                    // Dropped on a member row: insert at its position.
                    if (static_cast<size_t>(dst.a) >= s.categories.size() ||
                        static_cast<size_t>(dst.b) > s.categories[dst.a].members.size()) return;
                    size_t at = static_cast<size_t>(dst.b);
                    if (src.kind == 1 && src.a == dst.a && static_cast<size_t>(src.b) < at) --at;
                    removeSrc();
                    auto& m = s.categories[dst.a].members;
                    m.insert(m.begin() + static_cast<ptrdiff_t>(at), std::move(key));
                } else if (dst.kind == 0) {
                    // Dropped on a top-level row: become top-level there.
                    if (static_cast<size_t>(dst.a) >= s.items.size()) return;
                    size_t at = static_cast<size_t>(dst.a);
                    if (src.kind == 0 && static_cast<size_t>(src.a) < at) --at;
                    removeSrc();
                    s.items.insert(s.items.begin() + static_cast<ptrdiff_t>(at),
                                   TopItem{ false, 0, std::move(key) });
                } else {
                    return;
                }
                break;
            }
            case K::None:
                return;
        }
        Save();
    }

    // Modal popups. Returns the action to apply (Kind::None when idle).
    static PendingAction RenderModals() {
        PendingAction act;

        if (s_openAdd) { ImGui::OpenPopup("##catAdd"); s_openAdd = false; }
        if (s_openRename) { ImGui::OpenPopup("##catRename"); s_openRename = false; }
        if (s_openDelete) { ImGui::OpenPopup("##catDelete"); s_openDelete = false; }
        if (s_openSortAll) { ImGui::OpenPopup("##catSortAll"); s_openSortAll = false; }

        const ImGuiWindowFlags mf = ImGuiWindowFlags_AlwaysAutoResize;

        if (ImGui::BeginPopupModal("##catAdd", nullptr, mf)) {
            ImGui::TextUnformatted(Tr("$NewCategory_input").c_str());
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool submit = ImGui::InputText("##catAddName", s_nameBuf, sizeof(s_nameBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            const bool empty = s_nameBuf[0] == '\0';
            ImGui::BeginDisabled(empty);
            if ((ImGui::Button(Tr("$Add").c_str()) || submit) && !empty) {
                act.kind = PendingAction::Kind::AddCategory;
                act.text = s_nameBuf;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Cancel").c_str())) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("##catRename", nullptr, mf)) {
            ImGui::TextUnformatted(Tr("$RenameCategory_input").c_str());
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool submit = ImGui::InputText("##catRenName", s_nameBuf, sizeof(s_nameBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            const bool empty = s_nameBuf[0] == '\0';
            ImGui::BeginDisabled(empty);
            if ((ImGui::Button(Tr("$OK").c_str()) || submit) && !empty) {
                act.kind = PendingAction::Kind::RenameCategory;
                act.a = s_targetCat;
                act.text = s_nameBuf;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Cancel").c_str())) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("##catDelete", nullptr, mf)) {
            ImGui::TextUnformatted(Tr("$DeleteCategory_confirm").c_str());
            ImGui::TextDisabled("%s", Tr("$DeleteCategory_help").c_str());
            if (ImGui::Button(Tr("$OK").c_str())) {
                act.kind = PendingAction::Kind::DeleteCategory;
                act.a = s_targetCat;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Cancel").c_str())) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("##catSortAll", nullptr, mf)) {
            ImGui::TextUnformatted(Tr("$SortByName_confirm").c_str());
            if (ImGui::Button(Tr("$OK").c_str())) {
                act.kind = PendingAction::Kind::SortAll;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Cancel").c_str())) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        return act;
    }

    // Attaches a drag source and a drop target to the LAST submitted item.
    // `self` describes this row (also the drop destination); `label` is the
    // drag preview text. Fills `act` when a payload is dropped here.
    static void DragDropRow(const DragRef& self, const std::string& label, PendingAction& act) {
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kDragPayload, &self, sizeof(self));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
                if (p->DataSize == sizeof(DragRef)) {
                    act.kind = PendingAction::Kind::DragDrop;
                    std::memcpy(&act.src, p->Data, sizeof(DragRef));
                    act.dst = self;
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    // Up/down pair. Returns -1 / +1 / 0.
    static int MoveButtons(bool canUp, bool canDown) {
        int dir = 0;
        ImGui::BeginDisabled(!canUp);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) dir = -1;
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::BeginDisabled(!canDown);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) dir = +1;
        ImGui::EndDisabled();
        return dir;
    }

    void RenderImageControl(const std::string&, const std::string&) {
        EnsureLoaded();

        if (s.corrupt) {
            ImGui::TextWrapped("The saved category data (sCategories) could not be parsed. "
                               "Categorization is disabled and this editor is read-only so the "
                               "data is not overwritten. Fix or clear the value in "
                               "Data/MCM/Settings/MCMCategorizer.ini to start over.");
            return;
        }

        PendingAction act;

        // Toolbar
        if (ImGui::Button(Tr("$AddCategory").c_str())) {
            s_nameBuf[0] = '\0';
            s_openAdd = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("$NewCategory_help").c_str());
        ImGui::SameLine();
        if (ImGui::Button(Tr("$SortByName").c_str())) {
            s_openSortAll = true;
        }
        if (!s.enabled) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s: OFF)", Tr("$EnableCategorization").c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Drag rows to move or reorder (arrows work too)");
        ImGui::Separator();

        // Category / mod list
        for (size_t i = 0; i < s.items.size(); ++i) {
            const TopItem& item = s.items[i];
            ImGui::PushID(static_cast<int>(i));

            if (item.isCategory) {
                Category& c = s.categories[item.cat];

                {
                    const int dir = MoveButtons(i > 0, i + 1 < s.items.size());
                    if (dir != 0) { act.kind = PendingAction::Kind::MoveItem; act.a = i; act.dir = dir; }
                }
                ImGui::SameLine();

                const std::string header = s.wrapBefore + c.name + s.wrapAfter +
                                           "  (" + std::to_string(c.members.size()) + ")";
                const bool open = ImGui::TreeNodeEx("##cat",
                    ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanTextWidth,
                    "%s", header.c_str());
                // Categories drag to reorder; mods dropped here join the
                // category (appended at the end).
                DragDropRow(DragRef{ 2, static_cast<int>(item.cat), static_cast<int>(i) },
                            header, act);

                ImGui::SameLine();
                if (ImGui::SmallButton("Rename")) {
                    std::snprintf(s_nameBuf, sizeof(s_nameBuf), "%s", c.name.c_str());
                    s_targetCat = item.cat;
                    s_openRename = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("$RenameCategory_help").c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("A-Z")) {
                    act.kind = PendingAction::Kind::SortCategory;
                    act.a = item.cat;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("$SortCategory_help").c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) {
                    s_targetCat = item.cat;
                    s_openDelete = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("$DeleteCategory_help").c_str());

                if (open) {
                    for (size_t m = 0; m < c.members.size(); ++m) {
                        ImGui::PushID(static_cast<int>(m));
                        {
                            const int dir = MoveButtons(m > 0, m + 1 < c.members.size());
                            if (dir != 0) {
                                act.kind = PendingAction::Kind::MoveMember;
                                act.a = item.cat; act.b = m; act.dir = dir;
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("<")) {
                            act.kind = PendingAction::Kind::RemoveMember;
                            act.a = item.cat; act.b = m;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", Tr("$RemoveModFromCategory_help").c_str());
                        }
                        ImGui::SameLine();
                        const bool known = EntryForKey(c.members[m]) != nullptr;
                        const std::string disp = DisplayForKey(c.members[m]);
                        // Selectable (not Text) so the row is a drag handle
                        // with hover feedback and a full-width drop area.
                        if (!known) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        }
                        ImGui::Selectable(disp.c_str(), false);
                        if (!known) ImGui::PopStyleColor();
                        DragDropRow(DragRef{ 1, static_cast<int>(item.cat), static_cast<int>(m) },
                                    disp, act);
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            } else {
                // Top-level (uncategorized) mod row.
                {
                    const int dir = MoveButtons(i > 0, i + 1 < s.items.size());
                    if (dir != 0) { act.kind = PendingAction::Kind::MoveItem; act.a = i; act.dir = dir; }
                }
                ImGui::SameLine();

                if (!s.categories.empty()) {
                    // Labeled dropdown (a bare NoPreview arrow read as a
                    // confusing "down" button).
                    if (ImGui::BeginCombo("##assign", "Move to...",
                                          ImGuiComboFlags_WidthFitPreview)) {
                        for (size_t cIdx = 0; cIdx < s.categories.size(); ++cIdx) {
                            ImGui::PushID(static_cast<int>(cIdx));
                            if (ImGui::Selectable(s.categories[cIdx].name.c_str())) {
                                act.kind = PendingAction::Kind::AssignMember;
                                act.a = i; act.b = cIdx;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Move into a category (or drag the row onto one)");
                    }
                    ImGui::SameLine();
                }

                const bool known = EntryForKey(item.modKey) != nullptr;
                const std::string disp = DisplayForKey(item.modKey);
                if (!known) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                ImGui::Selectable(disp.c_str(), false);
                if (!known) {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Entry from the original MCM Categorizer that is not "
                                          "a translated menu here (kept intact when saving).");
                    }
                }
                DragDropRow(DragRef{ 0, static_cast<int>(i), 0 }, disp, act);
            }

            ImGui::PopID();
        }

        if (s.items.empty()) {
            ImGui::TextDisabled("No MCM menus registered.");
        }

        const PendingAction modalAct = RenderModals();
        if (modalAct.kind != PendingAction::Kind::None) act = modalAct;
        if (act.kind != PendingAction::Kind::None) {
            Apply(act);
        }
    }

}
