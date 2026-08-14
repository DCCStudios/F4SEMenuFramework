#include "Application.h"

std::vector<std::string> SplitString(const std::string& input, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream ss(input);
    std::string part;

    while (std::getline(ss, part, delimiter)) {
        parts.push_back(part);
    }

    return parts;
}
uint8_t GetToggleMode(std::string input) {

    std::transform(input.begin(), input.end(), input.begin(),
                   [](char c) { return static_cast<char>(std::toupper(c)); });

    const std::unordered_map<std::string, uint8_t> map = {
        {"SINGLEPRESS", 0},
        {"HOLD", 1},
        {"DOUBLEPRESS", 2},
        {"OFF", 3}
    };
    auto it = map.find(input);
    if (it != map.end()) {
        return it->second;
    } else {
        return 0x0;
    }
}

// Both directions are generated from the single table in DIK.h, so a key can
// no longer exist in one map and not the other. See that header for why the
// INI name strings are frozen and the identifiers are mixed-case.
//
// The maps are function-local statics: they used to be rebuilt on every call,
// which meant constructing a ~110-entry unordered_map each time GetKeyName ran
// — and the MCM keybind UI calls it per row, per frame.

int GetKeyBinding(std::string input, RE::INPUT_DEVICE device) {

    std::transform(input.begin(), input.end(), input.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });

    static const std::unordered_map<std::string, int> keymap = {
#define F4SEMF_DIK_NAME_TO_CODE(ident, name, code) { name, code },
        F4SEMF_DIK_LIST(F4SEMF_DIK_NAME_TO_CODE)
#undef F4SEMF_DIK_NAME_TO_CODE
    };

    static const std::unordered_map<std::string, int> keymapGP = {
#define F4SEMF_DIK_NAME_TO_CODE(ident, name, code) { name, code },
        F4SEMF_GAMEPAD_LIST(F4SEMF_DIK_NAME_TO_CODE)
#undef F4SEMF_DIK_NAME_TO_CODE
    };

    const auto& temp_map = device == RE::INPUT_DEVICE::kKeyboard ? keymap : keymapGP;
    auto it = temp_map.find(input);
    if (it != temp_map.end()) {
        return it->second;
    } else {
        return 0x0;
    }
}

std::string GetKeyName(int keyCode, RE::INPUT_DEVICE device) {
    static const std::unordered_map<int, std::string> keymap = {
#define F4SEMF_DIK_CODE_TO_NAME(ident, name, code) { code, name },
        F4SEMF_DIK_LIST(F4SEMF_DIK_CODE_TO_NAME)
#undef F4SEMF_DIK_CODE_TO_NAME
    };

    static const std::unordered_map<int, std::string> keymapGP = {
#define F4SEMF_DIK_CODE_TO_NAME(ident, name, code) { code, name },
        F4SEMF_GAMEPAD_LIST(F4SEMF_DIK_CODE_TO_NAME)
#undef F4SEMF_DIK_CODE_TO_NAME
    };

    const auto& temp_map = device == RE::INPUT_DEVICE::kKeyboard ? keymap : keymapGP;
    auto it = temp_map.find(keyCode);
    if (it != temp_map.end()) {
        return it->second;
    } else {
        return "UNKNOWN";
    }
}
