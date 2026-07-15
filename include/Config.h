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

    // Gamepad UX: which button glyph art the hint bar uses.
    // 0 = Xbox (default), 1 = PlayStation.
    static int GamepadGlyphStyle;
};