#pragma once


class Config {
    public:
    static void Init();
    static void Save();
    static unsigned int ToggleKey;
    static uint8_t ToggleMode;
    static unsigned int ToggleKeyGamePad;
    static uint8_t ToggleModeGamePad;
    static bool FreezeTimeOnMenu;
    static bool BlurBackgroundOnMenu;
    static int MenuStyle;
    static std::vector<std::string> MenuStyles;
    static std::string PrimaryFont;
    static bool EnableChinese;
    static bool EnableJapanese;
    static bool EnableKorean;
    static bool EnableCyrillic;
    static bool EnableThai;
    static float FontSizeSmall;
    static float FontSizeMedium;
    static float FontSizeBig;
    static void LoadStyle();

    // MCM Backwards Compatibility
    static bool MCMCompatEnabled;
    static bool MCMCompatWhenNativePresent;

    // Show the native "Edit Categories" entry even when the original m8r
    // MCM Categorizer mod is installed. Off by default: while that mod is
    // present its own page already opens our editor, so the extra entry is
    // hidden to avoid two doors to the same tool. With the mod removed, the
    // entry always shows regardless of this setting (it is the only door).
    static bool ShowCategorizerEditorAlways;

    // Gamepad UX: which button glyph art the hint bar uses.
    // 0 = Xbox (default), 1 = PlayStation.
    static int GamepadGlyphStyle;

    // Gamepad UX: while a controller is connected and the menu is open,
    // swallow keyboard input so stray key events (Steam Input emulation,
    // remote play, a bumped keyboard) can't fight controller navigation.
    // Keybind capture still reads the keyboard (it polls GetAsyncKeyState
    // directly), and the menu toggle key / ESC-to-close stay active.
    static bool DisableKeyboardWithGamepad;

    // Where the "F4SE FRAMEWORK" row is inserted in the pause menu list.
    // 0 = top (default), 1..N = that many rows down, -1 = bottom.
    // Applied the next time the pause menu opens.
    static int PauseMenuButtonPos;

    // Automatic backend translation of third-party plugin UI text
    // ([Localization] in the INI; see AutoTranslate.h).
    static bool AutoTranslatePlugins;
    static bool CaptureUIStrings;
};