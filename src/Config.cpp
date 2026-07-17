#include "Config.h"
#include "Application.h"
#include "Theme.h"
#include "Utils.h"
unsigned int Config::ToggleKey = 0x3B;
uint8_t Config::ToggleMode = 0;
unsigned int Config::ToggleKeyGamePad = 1;  // DPAD_UP (config code 1)
uint8_t Config::ToggleModeGamePad = 1;      // HOLD
bool Config::FreezeTimeOnMenu = true;
int Config::MenuStyle = 0;
std::vector<std::string> Config::MenuStyles;
bool Config::BlurBackgroundOnMenu = true;
std::string Config::PrimaryFont = "CN.ttf";  
bool Config::EnableChinese = true;                                
bool Config::EnableJapanese = false;
bool Config::EnableKorean = false;
bool Config::EnableCyrillic = false;
bool Config::EnableThai = false;
float Config::FontSizeSmall = 16.0f;
float Config::FontSizeMedium = 32.0f;
float Config::FontSizeBig = 64.0f;
bool Config::MCMCompatEnabled = true;
bool Config::MCMCompatWhenNativePresent = false;
int Config::GamepadGlyphStyle = 0;


void Config::Init() {
	const auto ini = new Ini("F4SEMenuFramework.ini");
    ini->SetSection("General");

    auto toggleKeyStr = ini->GetString("ToggleKey", "f1");
    ToggleKey = GetKeyBinding(toggleKeyStr);
    ToggleMode = GetToggleMode(ini->GetString("ToggleMode", "SinglePress"));
    logger::info("Config: ToggleKey='{}' -> scan code 0x{:X}, ToggleMode={}", toggleKeyStr, ToggleKey, ToggleMode);

    // Default gamepad activation: hold D-pad Up. Hold (rather than single
    // press) so the button can keep its normal gameplay function.
    ToggleKeyGamePad = GetKeyBinding(ini->GetString("ToggleKeyGamePad", "DPAD_UP"), RE::INPUT_DEVICE::kGamepad);
    ToggleModeGamePad = GetToggleMode(ini->GetString("ToggleModeGamePad", "Hold"));

    FreezeTimeOnMenu = ini->GetBool("FreezeTimeOnMenu", true);
    BlurBackgroundOnMenu = ini->GetBool("BlurBackgroundOnMenu", true);
    auto menuStyleStr = Utils::toUpperCase(ini->GetString("MenuStyle", "skyrimDefault"));

    MenuStyles = Theme::GetJsonFiles();
    Config::MenuStyle = Utils::indexOf(Config::MenuStyles, Utils::toUpperCase(menuStyleStr));

    ini->SetSection("Fonts");  
    PrimaryFont = ini->GetString("PrimaryFont", "MainFont.ttf");
    EnableChinese = ini->GetBool("EnableChinese", true);
    EnableJapanese = ini->GetBool("EnableJapanese", false);
    EnableKorean = ini->GetBool("EnableKorean", false);
    EnableCyrillic = ini->GetBool("EnableCyrillic", false);
    EnableThai = ini->GetBool("EnableThai", false);
    FontSizeSmall = ini->GetFloat("FontSizeSmall", 16.0f);
    FontSizeMedium = ini->GetFloat("FontSizeMedium", 32.0f);
    FontSizeBig = ini->GetFloat("FontSizeBig", 64.0f);

    ini->SetSection("MCMCompat");
    MCMCompatEnabled = ini->GetBool("Enabled", true);
    MCMCompatWhenNativePresent = ini->GetBool("MCMCompatWhenNativePresent", false);

    // Gamepad glyph platform: "xbox" (default) or "playstation"
    ini->SetSection("Gamepad");
    {
        std::string glyphStyle = ini->GetString("GlyphStyle", "xbox");
        std::transform(glyphStyle.begin(), glyphStyle.end(), glyphStyle.begin(), ::tolower);
        GamepadGlyphStyle = (glyphStyle == "playstation" || glyphStyle == "ps") ? 1 : 0;
    }

    delete ini;
    delete[] menuStyleStr;
}

void Config::Save() {
    const auto ini = new Ini("F4SEMenuFramework.ini");

    // General Section
    ini->SetSection("General");
    // Note: You'll need helper functions to convert key bindings and toggle modes back to strings
    // For now, these are placeholders - implement KeyBindingToString() and ToggleModeToString()
    // ini->SetString("ToggleKey", KeyBindingToString(ToggleKey).c_str());
    // ini->SetString("ToggleMode", ToggleModeToString(ToggleMode).c_str());
    // ini->SetString("ToggleKeyGamePad", KeyBindingToString(ToggleKeyGamePad).c_str());
    // ini->SetString("ToggleModeGamePad", ToggleModeToString(ToggleModeGamePad).c_str());

    ini->SetBool("FreezeTimeOnMenu", FreezeTimeOnMenu);
    ini->SetBool("BlurBackgroundOnMenu", BlurBackgroundOnMenu);


    if (ToggleMode == 0) {
        ini->SetString("ToggleMode", "SINGLEPRESS");
    }
    else if (ToggleMode == 1) {
        ini->SetString("ToggleMode", "HOLD");
    }
    else if (ToggleMode == 2) {
        ini->SetString("ToggleMode", "DOUBLEPRESS");
    } else if (ToggleMode == 3) {
        ini->SetString("ToggleMode", "OFF");
    }

    if (ToggleModeGamePad == 0) {
        ini->SetString("ToggleModeGamePad", "SINGLEPRESS");
    } else if (ToggleModeGamePad == 1) {
        ini->SetString("ToggleModeGamePad", "HOLD");
    } else if (ToggleModeGamePad == 2) {
        ini->SetString("ToggleModeGamePad", "DOUBLEPRESS");
    } else if (ToggleModeGamePad == 3) {
        ini->SetString("ToggleModeGamePad", "OFF");
    }

    ini->SetString("ToggleKey", GetKeyName(ToggleKey, RE::INPUT_DEVICE::kKeyboard).c_str());
    ini->SetString("ToggleKeyGamePad", GetKeyName(ToggleKeyGamePad, RE::INPUT_DEVICE::kGamepad).c_str());


    ini->SetString("MenuStyle", MenuStyles[MenuStyle].c_str());

    // Fonts Section
    ini->SetSection("Fonts");
    ini->SetString("PrimaryFont", PrimaryFont.c_str());
    ini->SetBool("EnableChinese", EnableChinese);
    ini->SetBool("EnableJapanese", EnableJapanese);
    ini->SetBool("EnableKorean", EnableKorean);
    ini->SetBool("EnableCyrillic", EnableCyrillic);
    ini->SetBool("EnableThai", EnableThai);

    ini->SetFloat("FontSizeSmall", FontSizeSmall);
    ini->SetFloat("FontSizeMedium", FontSizeMedium);
    ini->SetFloat("FontSizeBig", FontSizeBig);

    // MCMCompat Section
    ini->SetSection("MCMCompat");
    ini->SetBool("Enabled", MCMCompatEnabled);
    ini->SetBool("MCMCompatWhenNativePresent", MCMCompatWhenNativePresent);

    // Gamepad Section
    ini->SetSection("Gamepad");
    ini->SetString("GlyphStyle", GamepadGlyphStyle == 1 ? "playstation" : "xbox");

    // Save to file
    if (!ini->Save()) {
        // Log error if save fails
        // logger::error("Failed to save configuration to INI file");
    }

    delete ini;
}

void Config::LoadStyle() {
    
    Theme::LoadJsonStyle(MenuStyles[MenuStyle]);

}
