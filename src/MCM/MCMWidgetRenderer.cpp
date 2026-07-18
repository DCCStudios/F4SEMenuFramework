#include "MCM/MCMWidgetRenderer.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMPapyrusDispatch.h"
#include "MCM/MCMPapyrusAPI.h"
#include "MCM/MCMKeybindStore.h"
#include "MCM/MCMLiveSync.h"
#include "MCM/MCMConflictCheck.h"
#include "MCM/MCMScanner.h"
#include "MCM/SWFLibraryImage.h"
#include "MCM/SWFVectorMovie.h"
#include "MCM/FallUIHudEditor.h"
#include "F4SEMenuFramework.h"
#include "Application.h"
#include "GamepadInput.h"
#include "UI.h"  // UI::FuzzyMatch for the settings search
#include "imgui.h"
#include "TextureLoader.h"

#include <filesystem>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <map>
#include <memory>
#include <array>
#include <cmath>
#include <cctype>
#include <algorithm>

namespace MCMWidgetRenderer {

    // Persistent state for each control instance (current values, text buffers, etc.)
    struct ControlState {
        bool boolVal = false;
        int intVal = 0;
        float floatVal = 0.0f;
        std::string stringVal;
        char textBuf[512]{};
        bool initialized = false;
        MCMValueProvider::ProviderStatus lastStatus = MCMValueProvider::ProviderStatus::Available;

        // Resolved label for controls with textFromStringProperty (arrives
        // asynchronously from the Papyrus VM; empty until the read completes).
        std::string dynamicText;
    };

    // Per-mod render context
    struct ModRenderContext {
        std::string modName;
        MCMConfigParser::MCMModConfig config;
        std::map<std::string, ControlState> controlStates;
    };

    // Global storage for mod contexts
    static std::map<std::string, std::shared_ptr<ModRenderContext>> s_contexts;

    // Help text of the hovered / nav-focused control this frame (for the hint bar)
    static std::string s_focusedHelpText;

    // --- Hotkey capture state ---
    // When a hotkey control's rebind button is activated we enter capture mode:
    // the next keyboard key (or gamepad button) becomes the new binding.
    // ESC cancels; TAB (or gamepad X) unbinds.
    static bool s_captureActive = false;
    static std::string s_captureHotkeyId;    // HotkeyManager id ("MCM.<mod>.<id>")
    static std::string s_captureStateKey;    // renderer state key of the control
    static std::string s_captureModName;

    // --- Page open/close tracking for OnMCMMenuOpen/Close events ---
    static std::string s_renderedModThisFrame;  // set by RenderSlot each frame
    static std::string s_currentOpenMod;        // mod whose page is currently displayed

    // --- Settings search (right-panel search box in the main window) ---
    // Set each frame by the main window before the page renders; RenderPage
    // hides controls that don't match. Empty = no filtering.
    static std::string s_pageSearch;

    // --- Page thunk infrastructure (namespace scope) ---
    // Each MCM page gets assigned a slot. When the framework calls the thunk,
    // it looks up the context and renders the appropriate page.
    static constexpr size_t MAX_MCM_PAGES = 512;

    struct PageSlotInfo {
        std::string modName;
        size_t pageIdx = 0;
    };

    static PageSlotInfo s_pageSlots[MAX_MCM_PAGES]{};
    static size_t s_nextPageSlot = 0;

    // Forward declarations for internal render functions
    static void RenderPage(const MCMConfigParser::MCMPage& page, ModRenderContext& ctx);
    static void RenderControl(const MCMConfigParser::MCMControl& ctrl, ModRenderContext& ctx);

    // Callback for display-only hotkey registrations (hotkey controls that
    // have no keybinds.json action — the binding is shown/set but fires nothing).
    static void __stdcall NoOpHotkeyCallback() {}

    // FallUI's MCM configs place decorative "intro" image controls (animated
    // brand art such as M8r.View.FallUIHUDIntro — every FallUI mod ships one
    // on its landing page) purely as visual backdrops for the Scaleform MCM.
    // Flattened to a static texture by our SWF pipeline they just show up as
    // a giant meaningless image, so we skip them entirely.
    // M8r.View.FixFileDropdown is likewise skipped: it is an invisible AS3
    // patch shim for MCM's file dropdowns, which we implement natively.
    static bool IsSuppressedDecorativeImage(const std::string& className) {
        if (className == "M8r.View.FixFileDropdown") return true;
        return className.rfind("M8r.View.", 0) == 0 &&
               className.find("Intro") != std::string::npos;
    }

    // --- MCM Flash-library image resolution ---
    // Real MCM "image" controls reference an exported Flash symbol (className)
    // inside Data/MCM/Config/<libName>/lib.swf. The game renders those through
    // Scaleform; since our overlay is ImGui, we instead parse the SWF ourselves.
    // Resolution order (first hit wins; each step cached per libName|className):
    //   1./2. embedded bitmap in lib.swf / logo.swf   (SWFLibraryImage)
    //   3./4. vector shapes + timeline in lib.swf / logo.swf (SWFVectorMovie —
    //         static art is flattened to one texture; timeline animations get
    //         a movie handle composited live in RenderControl)
    // A miss on all paths is cached as a null result so the SWFs are never
    // re-parsed every frame.
    struct ResolvedImage {
        ImTextureID tex = 0;  // static texture (bitmap or flattened vector art)
        int width = 0;        // native layout size in px (movie px for vector)
        int height = 0;

        // Animated Flash symbol: the timeline + one texture per pre-rasterized
        // shape character. When set, `tex` is unused.
        std::shared_ptr<SWFVectorMovie::Movie> movie;
        std::unordered_map<const SWFVectorMovie::RasterChar*, ImTextureID> charTex;
    };

    // Attempts to decode + upload the class's bitmap from one SWF file.
    static ResolvedImage TryExtractFromSWF(const std::filesystem::path& swfPath, const std::string& className, const std::string& cacheKey) {
        ResolvedImage result;
        std::error_code ec;
        if (!std::filesystem::exists(swfPath, ec)) {
            return result;
        }
        if (auto img = SWFLibraryImage::Extract(swfPath, className)) {
            ImTextureID tex = TextureLoader::GetTextureFromMemory(cacheKey, img->width, img->height, img->rgba.data());
            if (tex) {
                result.tex = tex;
                result.width = img->width;
                result.height = img->height;
            }
        }
        return result;
    }

    // Attempts the vector/timeline renderer on one SWF. Static art becomes a
    // single flattened texture; animated movies keep the movie handle plus one
    // texture per rasterized character for live compositing.
    static ResolvedImage TryVectorFromSWF(const std::filesystem::path& swfPath, const std::string& className,
                                          const std::string& cacheKey, float targetWidthPx) {
        ResolvedImage result;
        std::error_code ec;
        if (!std::filesystem::exists(swfPath, ec)) {
            return result;
        }
        auto movie = SWFVectorMovie::Load(swfPath, className);
        if (!movie) {
            return result;
        }
        const float nativeW = movie->WidthPx();
        const float nativeH = movie->HeightPx();
        if (nativeW <= 0.0f || nativeH <= 0.0f) {
            return result;
        }
        result.width = static_cast<int>(nativeW + 0.5f);
        result.height = static_cast<int>(nativeH + 0.5f);

        if (movie->IsAnimated()) {
            // Upload one texture per pre-rasterized character; frames are
            // composited from these each render.
            for (const auto& rc : movie->Rasters()) {
                if (rc->image.width <= 0 || rc->image.height <= 0) {
                    continue;
                }
                const std::string texKey = cacheKey + "|char" + std::to_string(rc->charId);
                ImTextureID tex = TextureLoader::GetTextureFromMemory(
                    texKey, rc->image.width, rc->image.height, rc->image.rgba.data());
                if (tex) {
                    result.charTex[rc.get()] = tex;
                }
            }
            if (!result.charTex.empty()) {
                result.movie = std::move(movie);
            }
            return result;
        }

        // Static: flatten frame 0 at a scale matching the intended display
        // width (2x headroom for crisp downscaling, capped for VRAM sanity).
        float scale = targetWidthPx > 0.0f ? (targetWidthPx / nativeW) * 2.0f : 2.0f;
        scale = std::clamp(scale, 0.25f, 4.0f);
        auto img = movie->RenderFrame(0, scale);
        if (!img || img->width <= 0 || img->height <= 0) {
            return ResolvedImage{};
        }
        result.tex = TextureLoader::GetTextureFromMemory(cacheKey, img->width, img->height, img->rgba.data());
        if (!result.tex) {
            return ResolvedImage{};
        }
        return result;
    }

    static ResolvedImage ResolveLibraryImage(const std::string& libName, const std::string& className,
                                             float targetWidthPx) {
        static std::unordered_map<std::string, ResolvedImage> s_cache;

        const std::string key = libName + "|" + className;
        if (auto it = s_cache.find(key); it != s_cache.end()) {
            return it->second;
        }

        ResolvedImage result;
        if (!libName.empty() && !className.empty()) {
            const auto folder = MCMScanner::GetScanBasePath() / libName;

            // 1) Embedded bitmap in the library SWF itself. Mods whose banner
            //    is a bitmap (e.g. FallUI) resolve here directly by class name.
            result = TryExtractFromSWF(folder / "lib.swf", className, "mcmswf|" + key);

            // 2) M8r's generic "McmIntros" classes draw vector chrome and load a
            //    sibling logo.swf at runtime (LateImageLoader). lib.swf then has
            //    no bitmap, so fall back to that logo.swf and take its image.
            if (!result.tex) {
                result = TryExtractFromSWF(folder / "logo.swf", className, "mcmswf|logo|" + key);
            }

            // 3)/4) No embedded bitmap anywhere: rasterize the symbol's vector
            //    shapes (and timeline animation, if any) ourselves.
            if (!result.tex) {
                result = TryVectorFromSWF(folder / "lib.swf", className, "mcmswfvec|" + key, targetWidthPx);
            }
            if (!result.tex && !result.movie) {
                result = TryVectorFromSWF(folder / "logo.swf", className, "mcmswfvec|logo|" + key, targetWidthPx);
            }

            if (result.movie) {
                logger::info("[MCM] Loaded ANIMATED vector image '{}' for {} ({}x{} px, {} frames @ {:.1f} fps)",
                             className, libName, result.width, result.height,
                             result.movie->FrameCount(), result.movie->FrameRate());
            } else if (result.tex) {
                logger::info("[MCM] Loaded image '{}' for {} ({}x{})", className, libName, result.width, result.height);
            } else {
                logger::debug("[MCM] No renderable content for '{}' in {} (lib.swf/logo.swf)", className, libName);
            }
        }

        s_cache[key] = result;
        return result;
    }

    // Renders the page for a given slot index
    static void RenderSlot(size_t slotIdx) {
        if (slotIdx >= MAX_MCM_PAGES) return;
        const auto& slot = s_pageSlots[slotIdx];
        if (slot.modName.empty()) return;

        auto it = s_contexts.find(slot.modName);
        if (it == s_contexts.end()) return;

        // Record which mod's page is on screen this frame (used by OnFrameEnd
        // to fire OnMCMMenuOpen / OnMCMMenuClose on transitions).
        s_renderedModThisFrame = slot.modName;

        // If the native MCM's UI may be open right now (coexistence mode),
        // lock the page: both systems write the same settings files, so
        // concurrent editing would silently clobber one side's changes.
        const bool nativeMCMOpen = MCMConflictCheck::IsNativeMCMMenuOpen();
        if (nativeMCMOpen) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
            ImGui::TextWrapped("The Mod Configuration Menu is currently open.");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Close it before changing these settings here, otherwise the two menus would overwrite each other's values.");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::BeginDisabled();
        }

        auto& ctx = *it->second;
        if (slot.pageIdx < ctx.config.pages.size()) {
            RenderPage(ctx.config.pages[slot.pageIdx], ctx);
        }

        if (nativeMCMOpen) {
            ImGui::EndDisabled();
        }
    }

    // Template-based thunk generation at namespace scope
    template<size_t N>
    static void __stdcall PageThunk() {
        RenderSlot(N);
    }

    // Pre-instantiated thunk table using compile-time index sequence
    using PageRenderFn = void(__stdcall*)();

    template<size_t... Is>
    static constexpr std::array<PageRenderFn, sizeof...(Is)> MakeThunkTable(std::index_sequence<Is...>) {
        return {{ &PageThunk<Is>... }};
    }

    static auto s_pageThunks = MakeThunkTable(std::make_index_sequence<MAX_MCM_PAGES>{});

    // ------------------------------------------------------------------
    // State helpers
    // ------------------------------------------------------------------

    static std::string StateKeyFor(const MCMConfigParser::MCMControl& ctrl) {
        return ctrl.id.empty() ? ctrl.text : ctrl.id;
    }

    // ------------------------------------------------------------------
    // Text-input control support
    // MCM text inputs come in string/int/float flavors and ship with
    // inconsistent casing ("textInput", "textinputFloat", "textInputInt", ...),
    // so all detection is case-insensitive.
    // ------------------------------------------------------------------

    enum class TextInputKind { None, String, Int, Float };

    static std::string ToLowerCopy(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Formats a float for display in a text field without trailing zeros
    // (matches how the real MCM shows "0", "40", "0.5", etc.).
    static std::string FormatFloatClean(float v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
        return buf;
    }

    // Classifies a control as a text-input variant (or None). The numeric flavor
    // is taken from the type suffix when present, otherwise inferred from the
    // control's value source.
    static TextInputKind TextInputKindFor(const MCMConfigParser::MCMControl& ctrl) {
        const std::string t = ToLowerCopy(ctrl.type);
        if (t.rfind("textinput", 0) != 0) return TextInputKind::None;  // must start with "textinput"

        if (t == "textinputfloat") return TextInputKind::Float;
        if (t == "textinputint")   return TextInputKind::Int;

        // Base "textInput" (or any other textinput* variant): infer from source.
        switch (ctrl.valueSource.type) {
            case MCMConfigParser::SourceType::ModSettingFloat:
            case MCMConfigParser::SourceType::PropertyValueFloat:
            case MCMConfigParser::SourceType::GlobalValue:
                return TextInputKind::Float;
            case MCMConfigParser::SourceType::ModSettingInt:
            case MCMConfigParser::SourceType::PropertyValueInt:
                return TextInputKind::Int;
            default:
                return TextInputKind::String;
        }
    }

    static bool IsTextInput(const MCMConfigParser::MCMControl& ctrl) {
        return TextInputKindFor(ctrl) != TextInputKind::None;
    }

    // (Re)seeds a control's edit buffer from its cached value, formatted for the
    // control's numeric flavor.
    static void SeedTextBuf(const MCMConfigParser::MCMControl& ctrl, ControlState& state) {
        std::string s;
        switch (TextInputKindFor(ctrl)) {
            case TextInputKind::Float: s = FormatFloatClean(state.floatVal); break;
            case TextInputKind::Int:   s = std::to_string(state.intVal); break;
            default:                   s = state.stringVal; break;
        }
        strncpy_s(state.textBuf, s.c_str(), sizeof(state.textBuf) - 1);
    }

    // The mod whose settings INI a control actually reads/writes. Almost
    // always the owning mod, but the real MCM honors a per-control "modName"
    // override (FallUI Icon Library writes FallUI.ini's sItemSorterTagConfig).
    static const std::string& TargetModFor(const MCMConfigParser::MCMControl& ctrl,
                                           const ModRenderContext& ctx) {
        return ctrl.modNameOverride.empty() ? ctx.modName : ctrl.modNameOverride;
    }

    // Loads a control's value from the provider into its cached state.
    static void InitState(const MCMConfigParser::MCMControl& ctrl, ModRenderContext& ctx, ControlState& state) {
        if (state.initialized) return;
        if (ctrl.valueSource.type != MCMConfigParser::SourceType::None) {
            auto result = MCMValueProvider::GetValue(TargetModFor(ctrl, ctx), ctrl.valueSource);
            state.lastStatus = result.status;
            state.boolVal = result.boolVal;
            state.intVal = result.intVal;
            state.floatVal = result.floatVal;
            state.stringVal = result.stringVal;
            if (IsTextInput(ctrl)) {
                SeedTextBuf(ctrl, state);
            }
        }
        state.initialized = true;
    }

    // Returns the on/off state of the control that owns the given groupControl ID.
    // Controls on pages that haven't been rendered yet get their value pulled
    // from the provider on demand so conditions work across pages.
    static bool GetGroupState(ModRenderContext& ctx, int groupId) {
        for (const auto& page : ctx.config.pages) {
            for (const auto& other : page.controls) {
                if (other.groupControlId != groupId) continue;

                auto& otherState = ctx.controlStates[StateKeyFor(other)];
                InitState(other, ctx, otherState);
                // "On" is any truthy value — switchers store bool, but group
                // controls backed by ints/floats/globals count nonzero as on.
                return otherState.boolVal || otherState.intVal != 0 || otherState.floatVal != 0.0f;
            }
        }
        // Unknown group ID — treat as on so content isn't hidden by mistake.
        return true;
    }

    // Evaluates the standard MCM group condition (OR / AND / ONLY over
    // groupControl references). "page" is needed for ONLY semantics: all
    // groups NOT listed (on the same page) must be off.
    static bool EvaluateStandardGroupCondition(const MCMConfigParser::MCMControl& ctrl,
                                               const MCMConfigParser::MCMPage& page,
                                               ModRenderContext& ctx) {
        using Op = MCMConfigParser::GroupConditionOp;
        if (ctrl.groupConditionOp == Op::None || ctrl.groupConditionRefs.empty()) return true;

        auto isListed = [&](int id) {
            return std::find(ctrl.groupConditionRefs.begin(), ctrl.groupConditionRefs.end(), id)
                   != ctrl.groupConditionRefs.end();
        };

        switch (ctrl.groupConditionOp) {
            case Op::Or:
                for (int id : ctrl.groupConditionRefs) {
                    if (GetGroupState(ctx, id)) return true;
                }
                return false;

            case Op::And:
                for (int id : ctrl.groupConditionRefs) {
                    if (!GetGroupState(ctx, id)) return false;
                }
                return true;

            case Op::Only: {
                // All listed groups on...
                for (int id : ctrl.groupConditionRefs) {
                    if (!GetGroupState(ctx, id)) return false;
                }
                // ...and all OTHER groups defined on this page off.
                for (const auto& other : page.controls) {
                    if (other.groupControlId >= 0 && !isListed(other.groupControlId)) {
                        if (GetGroupState(ctx, other.groupControlId)) return false;
                    }
                }
                return true;
            }

            default:
                return true;
        }
    }

    // Non-standard comparison group condition (sourceSettingName/operator/compareValue)
    static bool EvaluateGroupCondition(const MCMConfigParser::GroupCondition& cond, ModRenderContext& ctx) {
        MCMConfigParser::ValueSource condSource;
        condSource.type = cond.sourceType;
        condSource.settingName = cond.sourceSettingName;

        auto result = MCMValueProvider::GetValue(ctx.modName, condSource);
        if (result.status != MCMValueProvider::ProviderStatus::Available) return true;

        std::string currentVal = result.stringVal;

        if (cond.operator_ == "==" || cond.operator_ == "=") {
            return currentVal == cond.compareValue;
        } else if (cond.operator_ == "!=") {
            return currentVal != cond.compareValue;
        } else if (cond.operator_ == ">") {
            try { return std::stof(currentVal) > std::stof(cond.compareValue); } catch (...) { return true; }
        } else if (cond.operator_ == "<") {
            try { return std::stof(currentVal) < std::stof(cond.compareValue); } catch (...) { return true; }
        } else if (cond.operator_ == ">=") {
            try { return std::stof(currentVal) >= std::stof(cond.compareValue); } catch (...) { return true; }
        } else if (cond.operator_ == "<=") {
            try { return std::stof(currentVal) <= std::stof(cond.compareValue); } catch (...) { return true; }
        }

        return true;
    }

    // Resolve options for a control (inline or from sharedOptions)
    static const std::vector<MCMConfigParser::OptionItem>& ResolveOptions(
        const MCMConfigParser::MCMControl& ctrl, const MCMConfigParser::MCMModConfig& config) {

        static std::vector<MCMConfigParser::OptionItem> empty;
        if (!ctrl.optionsFrom.empty()) {
            auto it = config.sharedOptions.find(ctrl.optionsFrom);
            if (it != config.sharedOptions.end()) return it->second;
        }
        return ctrl.options.empty() ? empty : ctrl.options;
    }

    // True when the control's value source stores strings (option TEXT is
    // stored); otherwise steppers/dropdowns store the selected INDEX as an
    // int — the real MCM behavior per the wiki's storage table.
    static bool IsStringSource(const MCMConfigParser::ValueSource& src) {
        return src.type == MCMConfigParser::SourceType::ModSettingString ||
               src.type == MCMConfigParser::SourceType::PropertyValueString;
    }

    // Number of decimal places implied by a slider step (0.05 -> 2, 0.1 -> 1, 1 -> 0).
    // Matches how the real MCM slider displays its value.
    static int DecimalsFromStep(float step) {
        if (step <= 0.0f) return 2;
        int decimals = 0;
        float s = step;
        while (decimals < 4 && std::fabs(s - std::round(s)) > 0.0001f) {
            s *= 10.0f;
            ++decimals;
        }
        return decimals;
    }

    // Maps a DIK scan code / gamepad config code to a friendly display name
    // for the hotkey control (keyboard bindings use DIK codes like real MCM).
    static std::string BindingDisplayName(unsigned int code) {
        if (code == 0) return "---";
        return GetKeyName(static_cast<int>(code), RE::INPUT_DEVICE::kKeyboard);
    }

    // ------------------------------------------------------------------
    // Hotkey capture — polls raw input while capture mode is active.
    // Keyboard: VK scan via GetAsyncKeyState -> DIK conversion (MapVirtualKey).
    // ESC cancels, TAB unbinds (mirrors the real MCM's remap flow where
    // Esc = cancel and Tab = unbind).
    // ------------------------------------------------------------------

    static void ProcessHotkeyCapture(ControlState& state) {
        if (!s_captureActive) return;

        // ESC — cancel capture, keep the old binding
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            s_captureActive = false;
            return;
        }

        // TAB — unbind
        if (GetAsyncKeyState(VK_TAB) & 0x8000) {
            HotkeyManager::SetBinding(s_captureHotkeyId.c_str(), 0);
            state.intVal = 0;
            s_captureActive = false;
            MCMPapyrusAPI::DispatchSettingChanged(s_captureModName, s_captureStateKey);
            return;
        }

        // Scan the useful VK range for a fresh key press and translate to DIK.
        // Skip mouse buttons (VK 1-6) and modifier-only virtual keys that don't
        // map to scan codes cleanly.
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk == VK_ESCAPE || vk == VK_TAB) continue;
            if (!(GetAsyncKeyState(vk) & 0x8000)) continue;

            UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            if (scan == 0) continue;

            // Extended keys (arrows, numpad enter, right ctrl/alt...) carry the
            // 0xE0 prefix in DirectInput; the game's DIK codes put them at 0x80+.
            switch (vk) {
                case VK_UP: scan = 0xC8; break;
                case VK_DOWN: scan = 0xD0; break;
                case VK_LEFT: scan = 0xCB; break;
                case VK_RIGHT: scan = 0xCD; break;
                case VK_HOME: scan = 0xC7; break;
                case VK_END: scan = 0xCF; break;
                case VK_PRIOR: scan = 0xC9; break;
                case VK_NEXT: scan = 0xD1; break;
                case VK_INSERT: scan = 0xD2; break;
                case VK_DELETE: scan = 0xD3; break;
                case VK_RCONTROL: scan = 0x9D; break;
                case VK_RMENU: scan = 0xB8; break;
                default: break;
            }

            // Conflict check: warn in the log but allow the bind, matching the
            // real MCM which shows a conflict prompt and rebinds on confirm.
            auto conflicts = HotkeyManager::GetConflicts(scan, s_captureHotkeyId.c_str());
            if (!conflicts.empty()) {
                logger::info("[MCMWidgetRenderer] Hotkey '{}' now shares scan code 0x{:X} with {} other binding(s)",
                    s_captureHotkeyId, scan, conflicts.size());
            }

            HotkeyManager::SetBinding(s_captureHotkeyId.c_str(), scan);
            state.intVal = static_cast<int>(scan);
            s_captureActive = false;
            MCMPapyrusAPI::DispatchSettingChanged(s_captureModName, s_captureStateKey);
            return;
        }
    }

    // ------------------------------------------------------------------
    // Control rendering
    // ------------------------------------------------------------------

    static void RenderControl(const MCMConfigParser::MCMControl& ctrl,
                              const MCMConfigParser::MCMPage& page,
                              ModRenderContext& ctx) {
        std::string stateKey = StateKeyFor(ctrl);
        auto& state = ctx.controlStates[stateKey];

        // Settings target: honors the per-control "modName" override
        // (used by controls that edit another mod's INI, e.g. FallUI).
        const std::string& targetMod = TargetModFor(ctrl, ctx);

        // Standard group condition (int / array / AND / OR / ONLY forms)
        if (!EvaluateStandardGroupCondition(ctrl, page, ctx)) {
            return;
        }

        // Non-standard comparison condition (object form)
        if (ctrl.groupCondition.has_value()) {
            if (!EvaluateGroupCondition(*ctrl.groupCondition, ctx)) {
                return;
            }
        }

        // Skip rendering for hiddenSwitcher — it only exists to define a groupControl
        if (ctrl.type == "hiddenSwitcher") {
            // Still need to initialize its value for groupCondition references
            InitState(ctrl, ctx, state);
            return;
        }

        // Initialize from value provider on first frame (or after RefreshMenu)
        InitState(ctrl, ctx, state);

        // --- Asynchronous Papyrus property value reads ---
        // Property-backed controls start from their config default (InitState);
        // the real value is fetched from the VM asynchronously. Kick the read
        // (no-op if already in flight) and absorb the result when it lands.
        const bool propertySource =
            ctrl.valueSource.type == MCMConfigParser::SourceType::PropertyValueBool ||
            ctrl.valueSource.type == MCMConfigParser::SourceType::PropertyValueInt ||
            ctrl.valueSource.type == MCMConfigParser::SourceType::PropertyValueFloat ||
            ctrl.valueSource.type == MCMConfigParser::SourceType::PropertyValueString;

        if (propertySource && state.lastStatus == MCMValueProvider::ProviderStatus::Available) {
            std::string asyncKey = ctx.modName + "\x1F" + stateKey;
            MCMValueProvider::RequestPropertyRead(asyncKey, ctrl.valueSource);

            MCMValueProvider::ValueResult live;
            if (MCMValueProvider::TryTakePropertyResult(asyncKey, live)) {
                if (live.status == MCMValueProvider::ProviderStatus::Available) {
                    state.boolVal = live.boolVal;
                    state.intVal = live.intVal;
                    state.floatVal = live.floatVal;
                    state.stringVal = live.stringVal;
                    if (IsTextInput(ctrl) && !ImGui::IsAnyItemActive()) {
                        SeedTextBuf(ctrl, state);
                    }
                } else {
                    // Read failed (e.g. property missing) — degrade the control
                    // so the user sees why instead of silently editing a dud.
                    state.lastStatus = live.status;
                }
            }
        }

        // --- Dynamic labels (textFromStringProperty) ---
        // The label itself comes from a Papyrus string property; read it the
        // same async way and fall back to the static text until it arrives.
        std::string displayText = ctrl.text;
        if (ctrl.textSource.has_value()) {
            std::string textKey = ctx.modName + "\x1F" + stateKey + "\x1F#text";
            MCMValueProvider::RequestPropertyRead(textKey, *ctrl.textSource);

            MCMValueProvider::ValueResult live;
            if (MCMValueProvider::TryTakePropertyResult(textKey, live) &&
                live.status == MCMValueProvider::ProviderStatus::Available) {
                state.dynamicText = live.stringVal;
            }
            if (!state.dynamicText.empty()) {
                displayText = state.dynamicText;
            }
        }

        bool degraded = (state.lastStatus != MCMValueProvider::ProviderStatus::Available);
        if (degraded) {
            ImGui::BeginDisabled();
        }

        // Helper: fire the control's action after a value change, and send the
        // OnMCMSettingChange external event exactly like the real MCM does.
        // Structured actions get the new value substituted into "{value}" params.
        auto fireValueChanged = [&](MCMPapyrusDispatch::ControlValue value) {
            if (ctrl.actionObj.has_value()) {
                MCMPapyrusDispatch::ExecuteStructuredAction(
                    *ctrl.actionObj, ctx.modName, ctrl.valueSource.sourceForm, value);
            } else if (!ctrl.action.empty()) {
                MCMPapyrusDispatch::ExecuteActionOnForm(ctrl.action, ctx.modName, ctrl.valueSource.sourceForm);
            }
            // The OnMCMSettingChange event carries the mod that OWNS the
            // setting (matters for per-control modName overrides).
            MCMPapyrusAPI::DispatchSettingChanged(targetMod, ctrl.id);
        };

        // Render by control type
        if (ctrl.type == "switcher") {
            if (ImGui::Checkbox(ctrl.text.c_str(), &state.boolVal)) {
                MCMValueProvider::SetBool(targetMod, ctrl.valueSource, state.boolVal);
                MCMValueProvider::FlushAll();
                fireValueChanged(state.boolVal);
            }
        } else if (ctrl.type == "slider") {
            // Real MCM sliders snap to their step and show decimals implied by
            // the step. Integer storage types render an int slider when the
            // step is a whole number.
            bool isFloatSource =
                ctrl.valueSource.type == MCMConfigParser::SourceType::ModSettingFloat ||
                ctrl.valueSource.type == MCMConfigParser::SourceType::PropertyValueFloat ||
                ctrl.valueSource.type == MCMConfigParser::SourceType::GlobalValue;

            float step = ctrl.sliderStep > 0.0f ? ctrl.sliderStep : 1.0f;

            if (isFloatSource) {
                int decimals = DecimalsFromStep(step);
                char fmt[16];
                snprintf(fmt, sizeof(fmt), "%%.%df", decimals);

                if (ImGui::SliderFloat(ctrl.text.c_str(), &state.floatVal,
                                       ctrl.sliderMin, ctrl.sliderMax, fmt)) {
                    // Snap to step grid, anchored at the slider minimum
                    state.floatVal = ctrl.sliderMin +
                        std::round((state.floatVal - ctrl.sliderMin) / step) * step;
                    state.floatVal = std::clamp(state.floatVal, ctrl.sliderMin, ctrl.sliderMax);
                    MCMValueProvider::SetFloat(targetMod, ctrl.valueSource, state.floatVal);
                }
                // Commit actions/events once the user releases the slider so we
                // don't spam Papyrus while dragging.
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    MCMValueProvider::FlushAll();
                    fireValueChanged(state.floatVal);
                }
            } else {
                int istep = std::max(1, static_cast<int>(step));
                if (ImGui::SliderInt(ctrl.text.c_str(), &state.intVal,
                    static_cast<int>(ctrl.sliderMin), static_cast<int>(ctrl.sliderMax))) {
                    int imin = static_cast<int>(ctrl.sliderMin);
                    state.intVal = imin + ((state.intVal - imin) / istep) * istep;
                    MCMValueProvider::SetInt(targetMod, ctrl.valueSource, state.intVal);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    MCMValueProvider::FlushAll();
                    fireValueChanged(state.intVal);
                }
            }

            // Gamepad X — reset the focused slider to its config default
            if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) &&
                !ctrl.valueSource.defaultValue.empty()) {
                try {
                    if (isFloatSource) {
                        state.floatVal = std::stof(ctrl.valueSource.defaultValue);
                        MCMValueProvider::SetFloat(targetMod, ctrl.valueSource, state.floatVal);
                        MCMValueProvider::FlushAll();
                        fireValueChanged(state.floatVal);
                    } else {
                        state.intVal = std::stoi(ctrl.valueSource.defaultValue);
                        MCMValueProvider::SetInt(targetMod, ctrl.valueSource, state.intVal);
                        MCMValueProvider::FlushAll();
                        fireValueChanged(state.intVal);
                    }
                } catch (...) {}
            }
        } else if (ctrl.type == "stepper" || ctrl.type == "dropdown") {
            const auto& options = ResolveOptions(ctrl, ctx.config);
            if (!options.empty()) {
                const bool stringStored = IsStringSource(ctrl.valueSource);
                const int count = static_cast<int>(options.size());

                // Resolve current selection. Real MCM steppers/dropdowns store
                // the selected INDEX for int sources; string sources store the
                // option's value/text.
                int currentIdx = 0;
                if (stringStored) {
                    for (int i = 0; i < count; ++i) {
                        if (options[i].value == state.stringVal || options[i].text == state.stringVal) {
                            currentIdx = i;
                            break;
                        }
                    }
                } else {
                    currentIdx = std::clamp(state.intVal, 0, count - 1);
                }

                // Commits a new selection to storage + fires actions/events
                auto commitSelection = [&](int newIdx) {
                    newIdx = std::clamp(newIdx, 0, count - 1);
                    if (stringStored) {
                        state.stringVal = options[newIdx].value.empty() ? options[newIdx].text
                                                                        : options[newIdx].value;
                        MCMValueProvider::SetString(targetMod, ctrl.valueSource, state.stringVal);
                        MCMValueProvider::FlushAll();
                        fireValueChanged(state.stringVal);
                    } else {
                        state.intVal = newIdx;
                        MCMValueProvider::SetInt(targetMod, ctrl.valueSource, newIdx);
                        MCMValueProvider::FlushAll();
                        fireValueChanged(newIdx);
                    }
                };

                if (ctrl.type == "stepper") {
                    // Real MCM stepper: one visible option with < > arrows that
                    // wrap around. Arrow buttons are individually nav-focusable
                    // so gamepad users can operate them with A.
                    ImGui::PushID(stateKey.c_str());
                    if (ImGui::ArrowButton("##left", ImGuiDir_Left)) {
                        commitSelection((currentIdx - 1 + count) % count);
                        currentIdx = (currentIdx - 1 + count) % count;
                    }
                    ImGui::SameLine();
                    // Show the current option centered in a fixed-width region
                    float optWidth = ImGui::CalcItemWidth() * 0.5f;
                    ImVec2 txtSize = ImGui::CalcTextSize(options[currentIdx].text.c_str());
                    float padX = std::max(0.0f, (optWidth - txtSize.x) * 0.5f);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
                    ImGui::TextUnformatted(options[currentIdx].text.c_str());
                    ImGui::SameLine(0.0f, padX + ImGui::GetStyle().ItemSpacing.x);
                    if (ImGui::ArrowButton("##right", ImGuiDir_Right)) {
                        commitSelection((currentIdx + 1) % count);
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(ctrl.text.c_str());
                    ImGui::PopID();
                } else {
                    // Dropdown — ImGui combo popup (nav: A opens, D-pad selects)
                    std::vector<const char*> labels;
                    labels.reserve(options.size());
                    for (const auto& opt : options) labels.push_back(opt.text.c_str());

                    int comboIdx = currentIdx;
                    if (ImGui::Combo(ctrl.text.c_str(), &comboIdx, labels.data(), count)) {
                        commitSelection(comboIdx);
                    }
                }
            }
        } else if (IsTextInput(ctrl)) {
            // Editable text field (string / int / float flavors). The real MCM
            // shows a value you type into and confirms; we mirror that by
            // committing on Enter OR when the field loses focus after an edit.
            const TextInputKind kind = TextInputKindFor(ctrl);

            ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
            if (kind == TextInputKind::Float || kind == TextInputKind::Int) {
                // CharsDecimal permits digits, sign and decimal point; the int
                // case is validated on commit via std::stoi.
                flags |= ImGuiInputTextFlags_CharsDecimal;
            }

            // Respect the config's maxLength (buffer capped at 512)
            size_t cap = std::min(sizeof(state.textBuf),
                                  static_cast<size_t>(std::max(1, ctrl.maxLength)) + 1);

            ImGui::PushID(stateKey.c_str());
            const bool entered = ImGui::InputText(ctrl.text.c_str(), state.textBuf, cap, flags);
            const bool commit = entered || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();

            if (commit) {
                switch (kind) {
                    case TextInputKind::Float: {
                        float v = state.floatVal;
                        try { v = std::stof(state.textBuf); } catch (...) {}
                        state.floatVal = v;
                        MCMValueProvider::SetFloat(targetMod, ctrl.valueSource, v);
                        MCMValueProvider::FlushAll();
                        SeedTextBuf(ctrl, state);   // normalize what's shown (e.g. "40." -> "40")
                        fireValueChanged(v);
                        break;
                    }
                    case TextInputKind::Int: {
                        int v = state.intVal;
                        try { v = std::stoi(state.textBuf); } catch (...) {}
                        state.intVal = v;
                        MCMValueProvider::SetInt(targetMod, ctrl.valueSource, v);
                        MCMValueProvider::FlushAll();
                        SeedTextBuf(ctrl, state);
                        fireValueChanged(v);
                        break;
                    }
                    default: {
                        state.stringVal = state.textBuf;
                        MCMValueProvider::SetString(targetMod, ctrl.valueSource, state.stringVal);
                        MCMValueProvider::FlushAll();
                        fireValueChanged(state.stringVal);
                        break;
                    }
                }
            }

            // Gamepad users: A focuses the field, but typing needs a keyboard.
            if (ImGui::IsItemFocused() && !ImGui::IsItemActive() && GamepadInput::IsControllerConnected()) {
                s_focusedHelpText = "Text entry requires the keyboard \xE2\x80\x94 press A/Enter to edit, then type.";
            }
        } else if (ctrl.type == "button") {
            const bool hasAction = ctrl.actionObj.has_value() || !ctrl.action.empty();

            if (ctrl.textSource.has_value() && !hasAction) {
                // Action-less info row whose text comes from a script property
                // (e.g. IAA's "Backpack Info" section) — the real MCM shows
                // these as plain display rows, so render text, not a button.
                if (displayText.empty()) {
                    ImGui::TextDisabled("...");
                } else {
                    ImGui::TextWrapped("%s", displayText.c_str());
                }
            } else {
                // Stable ImGui ID even when the label text changes at runtime
                std::string buttonLabel = displayText + "##" + stateKey;
                if (ImGui::Button(buttonLabel.c_str())) {
                    // Plain button press: no control value for {value} substitution
                    if (ctrl.actionObj.has_value()) {
                        MCMPapyrusDispatch::ExecuteStructuredAction(
                            *ctrl.actionObj, ctx.modName, ctrl.valueSource.sourceForm,
                            MCMPapyrusDispatch::ControlValue{});
                    } else {
                        MCMPapyrusDispatch::ExecuteActionOnForm(ctrl.action, ctx.modName, ctrl.valueSource.sourceForm);
                    }
                }
                const char* status = MCMPapyrusDispatch::GetStatusText();
                if (status && status[0] != '\0') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", status);
                }
            }
        } else if (ctrl.type == "section") {
            ImGui::SeparatorText(ctrl.text.c_str());
        } else if (ctrl.type == "text") {
            ImGui::TextWrapped("%s", displayText.c_str());
        } else if (ctrl.type == "spacer") {
            // numLines or height per the MCM wiki
            if (ctrl.spacerHeight > 0.0f) {
                ImGui::Dummy(ImVec2(1.0f, ctrl.spacerHeight));
            } else {
                for (int i = 0; i < std::max(1, ctrl.spacerLines); ++i) {
                    ImGui::Spacing();
                }
            }
        } else if (ctrl.type == "image" &&
                   FallUIHudEditor::HandlesImageControl(ctrl.imageLibName, ctrl.imageClassName)) {
            // FallUI ships full ActionScript applications inside MCM "image"
            // controls (the drag-and-drop HUD layout editor and the Icon
            // Library preset manager). Those need a live Scaleform stage we
            // don't have, so a native ImGui recreation takes over the control.
            FallUIHudEditor::RenderImageControl(ctrl.imageLibName, ctrl.imageClassName);
        } else if (ctrl.type == "image" && IsSuppressedDecorativeImage(ctrl.imageClassName)) {
            // Intentionally rendered as nothing — see IsSuppressedDecorativeImage.
        } else if (ctrl.type == "image") {
            // Resolve the texture from either the Flash library (MCM's real
            // form) or a loose image file. `nativeW/H` hold the source image's
            // pixel size (used for aspect when the config omits width/height).
            ImTextureID texId = 0;
            int nativeW = 0, nativeH = 0;
            ResolvedImage resolved;

            if (!ctrl.imageLibName.empty() && !ctrl.imageClassName.empty()) {
                resolved = ResolveLibraryImage(ctrl.imageLibName, ctrl.imageClassName,
                                               ctrl.imageWidth > 0.0f ? ctrl.imageWidth : 0.0f);
                texId = resolved.tex;
                nativeW = resolved.width;
                nativeH = resolved.height;
            }

            // Fall back to a loose image file if provided (dds/png/svg). Paths
            // are resolved relative to the game root (MCM paths are Data-relative).
            if (!texId && !resolved.movie && !ctrl.imagePath.empty()) {
                std::string path = ctrl.imagePath;
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) {
                    auto dataRel = (std::filesystem::current_path() / "Data" / ctrl.imagePath);
                    if (std::filesystem::exists(dataRel, ec)) {
                        path = dataRel.string();
                    }
                }
                ImVec2 want(ctrl.imageWidth > 0 ? ctrl.imageWidth : 256.0f,
                            ctrl.imageHeight > 0 ? ctrl.imageHeight : 256.0f);
                texId = TextureLoader::GetTexture(path.c_str(), want);
            }

            if (texId || resolved.movie) {
                // Preferred display size: the control's configured width/height
                // (what native MCM lays the symbol out at). Fall back to the
                // source image's native size, then a square default.
                float w = ctrl.imageWidth > 0.0f ? ctrl.imageWidth : (nativeW > 0 ? static_cast<float>(nativeW) : 256.0f);
                float h = ctrl.imageHeight > 0.0f ? ctrl.imageHeight : (nativeH > 0 ? static_cast<float>(nativeH) : 256.0f);

                // Never overflow the content pane: scale down proportionally to
                // the available width so wide banners stay fully visible.
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > 4.0f && w > avail) {
                    const float s = avail / w;
                    w *= s;
                    h *= s;
                }

                if (resolved.movie) {
                    // Timeline-animated Flash symbol: composite this frame's
                    // quads (per-character textures + per-frame transforms)
                    // straight into the window draw list. The frame counter is
                    // wall-clock based; each timeline wraps itself, so nested
                    // sprite loops keep running past the root's length.
                    const int frame = static_cast<int>(ImGui::GetTime() * resolved.movie->FrameRate());
                    static std::vector<SWFVectorMovie::FrameDraw> s_draws;
                    resolved.movie->BuildFrameDraws(frame, s_draws);

                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const float sx = nativeW > 0 ? w / static_cast<float>(nativeW) : 1.0f;
                    const float sy = nativeH > 0 ? h / static_cast<float>(nativeH) : 1.0f;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    for (const auto& fd : s_draws) {
                        auto texIt = resolved.charTex.find(fd.image);
                        if (texIt == resolved.charTex.end()) {
                            continue;
                        }
                        const ImU32 tint = IM_COL32(static_cast<int>(fd.mulR * 255.0f),
                                                    static_cast<int>(fd.mulG * 255.0f),
                                                    static_cast<int>(fd.mulB * 255.0f),
                                                    static_cast<int>(fd.mulA * 255.0f));
                        dl->AddImageQuad(texIt->second,
                                         ImVec2(pos.x + fd.x0 * sx, pos.y + fd.y0 * sy),
                                         ImVec2(pos.x + fd.x1 * sx, pos.y + fd.y1 * sy),
                                         ImVec2(pos.x + fd.x2 * sx, pos.y + fd.y2 * sy),
                                         ImVec2(pos.x + fd.x3 * sx, pos.y + fd.y3 * sy),
                                         ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
                    }
                    ImGui::Dummy(ImVec2(w, h));  // reserve the layout space
                } else {
                    ImGui::Image(texId, ImVec2(w, h));
                }
            } else if (!ctrl.imageClassName.empty() || !ctrl.imagePath.empty()) {
                // Vector/ActionScript-drawn symbol with no extractable bitmap,
                // or a missing asset: show a compact, correctly-sized placeholder
                // so page layout still reads sensibly.
                const char* label = !ctrl.text.empty() ? ctrl.text.c_str()
                                    : !ctrl.imageClassName.empty() ? ctrl.imageClassName.c_str()
                                                                   : ctrl.imagePath.c_str();
                ImGui::TextDisabled("[Image: %s]", label);
            }
        } else if (ctrl.type == "hotkey" || ctrl.type == "keyinput") {
            // Rebindable hotkey control with the real MCM remap flow:
            // click/A -> "Press a key..." -> next key binds; ESC cancels; TAB unbinds.
            // Binding is stored through HotkeyManager under "MCM.<mod>.<id>",
            // the same id the keybind translator registered from keybinds.json.
            std::string hotkeyId = "MCM." + ctx.modName + "." + ctrl.id;

            // Hotkey controls without a keybinds.json entry were never
            // registered — register a display-only binding on first render so
            // the key still shows, rebinds, and syncs to MCM's Keybinds.json.
            if (!HotkeyManager::IsRegistered(hotkeyId.c_str())) {
                HotkeyManager::Register(hotkeyId.c_str(), 0, &NoOpHotkeyCallback);
                MCMKeybindStore::RegisterMapping(hotkeyId, ctx.modName, ctrl.id);
                if (auto saved = MCMKeybindStore::GetSavedDIK(ctx.modName, ctrl.id); saved.has_value()) {
                    HotkeyManager::ImportBinding(hotkeyId.c_str(), *saved);
                }
            }

            // Pull the live binding from the hotkey system (it owns persistence)
            unsigned int binding = HotkeyManager::GetBinding(hotkeyId.c_str());
            state.intVal = static_cast<int>(binding);

            // Whether the real MCM.dll is installed alongside us. When it is,
            // rebinding here still reaches MCM: MCM applies its keybinds from
            // Data/MCM/Settings/Keybinds.json at game *load*, and our rebind
            // writes that exact file in MCM's Windows-VK format (see
            // MCMKeybindStore). The change therefore can't apply live inside the
            // already-running MCM — but it is picked up the next time a save is
            // loaded. We keep the control fully interactive and just tell the
            // user about the load-time apply so expectations are correct.
            const bool mcmPresent = MCMConflictCheck::IsNativeMCMPresent();

            bool capturingThis = s_captureActive && s_captureHotkeyId == hotkeyId;
            std::string buttonLabel = capturingThis
                ? "[ Press a key... (Esc: cancel / Tab: unbind) ]"
                : "[ " + BindingDisplayName(binding) + " ]";

            ImGui::PushID(stateKey.c_str());
            if (ImGui::Button(buttonLabel.c_str())) {
                if (!capturingThis) {
                    s_captureActive = true;
                    s_captureHotkeyId = hotkeyId;
                    s_captureStateKey = ctrl.id;
                    s_captureModName = ctx.modName;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(ctrl.text.c_str());
            ImGui::PopID();

            // Gamepad X — unbind the focused hotkey control directly
            if (!capturingThis && ImGui::IsItemFocused() &&
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) && binding != 0) {
                HotkeyManager::SetBinding(hotkeyId.c_str(), 0);
                state.intVal = 0;
                MCMPapyrusAPI::DispatchSettingChanged(ctx.modName, ctrl.id);
            }

            if (capturingThis) {
                ProcessHotkeyCapture(state);
            }

            // Coexistence feedback: a rebind is (1) written to MCM's
            // Keybinds.json (applies at the next save load regardless) and
            // (2) queued for a live push into the running MCM through its
            // root.mcm Scaleform object — which only exists on the pause-menu
            // movie, so the push lands the next time the pause menu opens.
            // Tell the user which of those states this control is in.
            if (mcmPresent) {
                const auto sync = MCMLiveSync::GetStatus(ctx.modName, ctrl.id);
                if (sync != MCMLiveSync::Status::None) {
                    ImGui::Indent();
                    if (sync == MCMLiveSync::Status::Synced) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.90f, 0.45f, 1.0f));
                        ImGui::TextWrapped("Synced to MCM \xE2\x80\x94 active now.");
                    } else if (sync == MCMLiveSync::Status::Pending) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
                        ImGui::TextWrapped("Saved. Open the game's pause menu (ESC) once to sync the "
                            "running MCM \xE2\x80\x94 otherwise it applies on the next save load.");
                    } else {  // Failed
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
                        ImGui::TextWrapped("Saved to MCM's Keybinds.json \xE2\x80\x94 applies on the next "
                            "save load (live sync into the running MCM didn't succeed).");
                    }
                    ImGui::PopStyleColor();
                    ImGui::Unindent();
                }
            }
        } else if (ctrl.type == "positioner") {
            // Simplified positioner: the real MCM opens a draggable marker
            // window (POS_WINDOW); we expose the same value range via sliders.
            ImGui::TextUnformatted(ctrl.text.c_str());
            if (ImGui::SliderFloat((ctrl.text + " X").c_str(), &state.floatVal, ctrl.posMinX, ctrl.posMaxX)) {
                MCMValueProvider::SetFloat(targetMod, ctrl.valueSource, state.floatVal);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                MCMValueProvider::FlushAll();
                fireValueChanged(state.floatVal);
            }
        } else if (ctrl.type == "dropdownFiles") {
            // Directory-listing dropdown (real MCM control type used by
            // FallUI). Enumerates <game>/<path> for files matching the mask;
            // the stored value is the file NAME with extension (verified from
            // FallUI.ini: "sItemSorterTagConfig = Auto-detect.xml").
            //
            // The listing is cached per path|mask and refreshed lazily every
            // few seconds so newly dropped files show up without reopening.
            struct FileListCache {
                std::vector<std::string> names;
                double scannedAt = -1e9;
            };
            static std::unordered_map<std::string, FileListCache> s_fileLists;

            const std::string cacheKey = ctrl.filesPath + "|" + ctrl.filesMask;
            auto& cache = s_fileLists[cacheKey];
            if (ImGui::GetTime() - cache.scannedAt > 3.0) {
                cache.scannedAt = ImGui::GetTime();
                cache.names.clear();

                // Wildcard mask -> case-insensitive suffix/pattern match.
                // Masks in the wild are "*.xml"-style; support prefix*suffix too.
                auto matchesMask = [](const std::string& name, const std::string& mask) {
                    if (mask.empty() || mask == "*" || mask == "*.*") return true;
                    auto lower = [](std::string s) {
                        std::transform(s.begin(), s.end(), s.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return s;
                    };
                    const std::string n = lower(name);
                    const std::string m = lower(mask);
                    const size_t star = m.find('*');
                    if (star == std::string::npos) return n == m;
                    const std::string prefix = m.substr(0, star);
                    const std::string suffix = m.substr(star + 1);
                    return n.size() >= prefix.size() + suffix.size() &&
                           n.compare(0, prefix.size(), prefix) == 0 &&
                           n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
                };

                std::error_code ec;
                // MCM paths are game-root relative (e.g. "data\\Interface\\ItemSorter")
                std::filesystem::path dir = std::filesystem::current_path() / ctrl.filesPath;
                for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                    if (!it->is_regular_file(ec)) continue;
                    std::string name = it->path().filename().string();
                    if (matchesMask(name, ctrl.filesMask)) {
                        cache.names.push_back(std::move(name));
                    }
                }
                std::sort(cache.names.begin(), cache.names.end());
            }

            if (cache.names.empty()) {
                ImGui::TextDisabled("%s: no files found (%s\\%s)", ctrl.text.c_str(),
                                    ctrl.filesPath.c_str(), ctrl.filesMask.c_str());
            } else {
                // Current selection by stored file name; unknown values show
                // as-is so we never silently rewrite a user's setting.
                int currentIdx = -1;
                for (int i = 0; i < static_cast<int>(cache.names.size()); ++i) {
                    if (cache.names[i] == state.stringVal) { currentIdx = i; break; }
                }

                const char* previewText = currentIdx >= 0 ? cache.names[currentIdx].c_str()
                                          : (state.stringVal.empty() ? "(none)" : state.stringVal.c_str());
                ImGui::PushID(stateKey.c_str());
                if (ImGui::BeginCombo(ctrl.text.c_str(), previewText)) {
                    for (int i = 0; i < static_cast<int>(cache.names.size()); ++i) {
                        const bool selected = (i == currentIdx);
                        if (ImGui::Selectable(cache.names[i].c_str(), selected)) {
                            state.stringVal = cache.names[i];
                            MCMValueProvider::SetString(targetMod, ctrl.valueSource, state.stringVal);
                            MCMValueProvider::FlushAll();
                            fireValueChanged(state.stringVal);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextDisabled("[%s: %s]", ctrl.type.c_str(), ctrl.text.c_str());
        }

        if (degraded) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                std::string tooltip = MCMValueProvider::GetStatusTooltip(state.lastStatus, ctrl.valueSource);
                if (!tooltip.empty()) {
                    ImGui::SetTooltip("%s", tooltip.c_str());
                }
            }
        } else if (!ctrl.help.empty()) {
            // Mouse users get a tooltip; gamepad/keyboard users get the help
            // text mirrored into the hint-bar footer for the focused control
            // (the real MCM shows help in a bottom panel on selection).
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", ctrl.help.c_str());
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
                s_focusedHelpText = ctrl.help;
            }
        }
    }

    // --- Page rendering ---
    static void RenderPage(const MCMConfigParser::MCMPage& page, ModRenderContext& ctx) {
        const auto& ctrls = page.controls;

        // No search active — render everything as-is.
        if (s_pageSearch.empty()) {
            for (const auto& ctrl : ctrls) {
                RenderControl(ctrl, page, ctx);
            }
            return;
        }

        // Settings search active. A control matches on its label, help text
        // or id (fuzzy — see UI::FuzzyMatch). Section headers group the
        // controls that follow them, so:
        //  - a matching section header shows its ENTIRE section, and
        //  - a section header stays visible when any control inside matches
        //    (results keep their context).
        // hiddenSwitcher controls always pass through: they render nothing
        // but seed the group-condition state other controls depend on.
        auto ctrlMatches = [](const MCMConfigParser::MCMControl& c) {
            return UI::FuzzyMatch(s_pageSearch.c_str(), c.text.c_str()) ||
                   (!c.help.empty() && UI::FuzzyMatch(s_pageSearch.c_str(), c.help.c_str())) ||
                   (!c.id.empty() && UI::FuzzyMatch(s_pageSearch.c_str(), c.id.c_str()));
        };

        std::vector<char> show(ctrls.size(), 0);
        bool anyShown = false;
        size_t i = 0;
        while (i < ctrls.size()) {
            if (ctrls[i].type == "section") {
                size_t end = i + 1;
                while (end < ctrls.size() && ctrls[end].type != "section") ++end;

                const bool headerMatch = ctrlMatches(ctrls[i]);
                bool anyChildMatch = false;
                for (size_t j = i + 1; j < end; ++j) {
                    if (ctrls[j].type == "hiddenSwitcher") {
                        show[j] = 1;  // always processed, renders nothing
                        continue;
                    }
                    if (headerMatch || ctrlMatches(ctrls[j])) {
                        show[j] = 1;
                        anyChildMatch = true;
                    }
                }
                show[i] = (headerMatch || anyChildMatch) ? 1 : 0;
                if (show[i]) anyShown = true;
                i = end;
            } else {
                show[i] = (ctrls[i].type == "hiddenSwitcher" || ctrlMatches(ctrls[i])) ? 1 : 0;
                if (show[i] && ctrls[i].type != "hiddenSwitcher") anyShown = true;
                ++i;
            }
        }

        for (size_t k = 0; k < ctrls.size(); ++k) {
            if (show[k]) {
                RenderControl(ctrls[k], page, ctx);
            }
        }

        if (!anyShown) {
            ImGui::TextDisabled("No settings match \"%s\".", s_pageSearch.c_str());
        }
    }

    // ------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------

    void RegisterMod(const MCMConfigParser::MCMModConfig& config, const std::string& modName) {
        auto context = std::make_shared<ModRenderContext>();
        context->modName = modName;
        context->config = config;
        s_contexts[modName] = context;

        for (size_t i = 0; i < config.pages.size(); ++i) {
            if (s_nextPageSlot >= MAX_MCM_PAGES) {
                logger::warn("[MCMWidgetRenderer] Maximum page slots ({}) exhausted", MAX_MCM_PAGES);
                break;
            }

            const auto& page = config.pages[i];
            // Grouped under "MCM Mod Configs (Legacy)" in the F4SE Menu Framework tree.
            // The tree renderer gives this category a distinct accent color so
            // translated MCM menus are visually separate from native pages.
            std::string path;
            if (config.pages.size() == 1) {
                path = "MCM Mod Configs (Legacy)/" + config.displayName;
            } else {
                path = "MCM Mod Configs (Legacy)/" + config.displayName + "/" + page.pageDisplayName;
            }

            size_t slot = s_nextPageSlot++;
            s_pageSlots[slot].modName = modName;
            s_pageSlots[slot].pageIdx = i;

            AddSectionItem(path.c_str(), s_pageThunks[slot]);
            logger::debug("[MCMWidgetRenderer] Registered page '{}' (slot {})", path, slot);
        }

        logger::info("[MCMWidgetRenderer] Registered mod '{}' with {} page(s)",
            config.displayName, config.pages.size());
    }

    void UnregisterMod(const std::string& modName) {
        s_contexts.erase(modName);
    }

    void InvalidateAllStates() {
        // Called from the Papyrus VM thread (MCM.RefreshMenu native) — control
        // states are only mutated on the render thread during RenderControl,
        // and clearing `initialized` triggers a re-read there.
        for (auto& [name, ctx] : s_contexts) {
            if (!ctx) continue;
            for (auto& [key, state] : ctx->controlStates) {
                state.initialized = false;
            }
        }
        // Also forget completed async property reads so they re-dispatch.
        MCMValueProvider::InvalidateAsyncPropertyReads();
        // The FallUI editor recreation caches its whole session (widgets,
        // profiles, global settings) — drop it so it re-reads the INIs too.
        FallUIHudEditor::ResetSession();
        logger::debug("[MCMWidgetRenderer] All control states invalidated (RefreshMenu)");
    }

    void OnFrameEnd() {
        // Fire OnMCMMenuOpen / OnMCMMenuClose on page transitions. RenderSlot
        // records the displayed mod each frame; when it changes (or nothing is
        // rendered because the menu closed / another page was selected) we
        // dispatch close for the old page and open for the new one — mirroring
        // the real MCM's per-mod menu events.
        if (s_renderedModThisFrame != s_currentOpenMod) {
            if (!s_currentOpenMod.empty()) {
                MCMPapyrusAPI::DispatchMenuClose(s_currentOpenMod);
                // Leaving a page destroys the original Flash apps too — drop
                // the FallUI editor session so reopening reloads from the INIs.
                FallUIHudEditor::ResetSession();
            }
            if (!s_renderedModThisFrame.empty()) {
                MCMPapyrusAPI::DispatchMenuOpen(s_renderedModThisFrame);

                // Fresh page open: re-read the mod's values (the real MCM
                // reloads on every menu open too — scripts may have changed
                // globals/properties since the page was last displayed).
                auto ctxIt = s_contexts.find(s_renderedModThisFrame);
                if (ctxIt != s_contexts.end() && ctxIt->second) {
                    for (auto& [key, state] : ctxIt->second->controlStates) {
                        state.initialized = false;
                    }
                }
                MCMValueProvider::InvalidateAsyncPropertyReads();
            }
            s_currentOpenMod = s_renderedModThisFrame;
        }
        s_renderedModThisFrame.clear();

        // Help text is re-collected every frame by the hovered/focused control.
        // The hint bar reads it during the frame (it renders after the page
        // content), so clearing here is safe.
        s_focusedHelpText.clear();
    }

    const std::string& GetFocusedHelpText() {
        return s_focusedHelpText;
    }

    bool IsHotkeyCaptureActive() {
        return s_captureActive;
    }

    void CancelHotkeyCapture() {
        s_captureActive = false;
    }

    bool IsMCMPageFunction(void(__stdcall* fn)()) {
        if (!fn) return false;
        for (size_t i = 0; i < s_nextPageSlot && i < MAX_MCM_PAGES; ++i) {
            if (s_pageThunks[i] == fn) return true;
        }
        return false;
    }

    void SetPageSearchFilter(const char* text) {
        s_pageSearch = text ? text : "";
    }

} // namespace MCMWidgetRenderer
