#pragma once
#include "SimpleIni.h"
std::vector<std::string> SplitString(const std::string& input, char delimiter);
uint8_t GetToggleMode(std::string input);
int GetKeyBinding(std::string input, RE::INPUT_DEVICE device = RE::INPUT_DEVICE::kKeyboard);
std::string GetKeyName(int keyCode, RE::INPUT_DEVICE device);
class Ini {
private:
    CSimpleIniA _ini;
    const char* _section;
    std::string _filename;
    bool opened;

public:
    Ini(const char* filename) {
        _ini.SetUnicode();
        _filename = "Data/F4SE/Plugins/" + std::string(filename);
        opened = _ini.LoadFile(_filename.c_str()) >= 0;
    }

    void SetSection(const char* section) { _section = section; }

    // Getter functions
    const char* GetString(const char* key, const char* def = "") const {
        if (!opened) return def;
        return _ini.GetValue(_section, key, def);
    }

    float GetFloat(const char* key, float def = 0.0f) const {
        if (!opened) return def;
        return static_cast<float>(_ini.GetDoubleValue(_section, key, def));
    }

    int GetInt(const char* key, int def = 0) const {
        if (!opened) return def;
        return static_cast<int>(_ini.GetLongValue(_section, key, def));
    }

    bool GetBool(const char* key, bool def = false) const {
        if (!opened) return def;
        return _ini.GetBoolValue(_section, key, def);
    }

    // Setter functions
    bool SetString(const char* key, const char* value) {
        if (!opened) return false;
        return _ini.SetValue(_section, key, value) >= 0;
    }

    bool SetFloat(const char* key, float value) {
        if (!opened) return false;
        return _ini.SetDoubleValue(_section, key, static_cast<double>(value)) >= 0;
    }

    bool SetInt(const char* key, int value) {
        if (!opened) return false;
        return _ini.SetLongValue(_section, key, static_cast<long>(value)) >= 0;
    }

    bool SetBool(const char* key, bool value) {
        if (!opened) return false;
        return _ini.SetBoolValue(_section, key, value) >= 0;
    }

    // Save function
    bool Save() {
        if (!opened) return false;
        return _ini.SaveFile(_filename.c_str()) >= 0;
    }

    // Check if INI was successfully opened
    bool IsOpened() const { return opened; }
};
