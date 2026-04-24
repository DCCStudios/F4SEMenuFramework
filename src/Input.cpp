#include "Input.h"
#include "imgui.h"

// DirectInput scan codes (same as Bethesda engine button codes for keyboard)
namespace DIK {
    enum : std::uint32_t {
        kEscape       = 0x01,
        kNum1         = 0x02,
        kNum2         = 0x03,
        kNum3         = 0x04,
        kNum4         = 0x05,
        kNum5         = 0x06,
        kNum6         = 0x07,
        kNum7         = 0x08,
        kNum8         = 0x09,
        kNum9         = 0x0A,
        kNum0         = 0x0B,
        kMinus        = 0x0C,
        kEquals       = 0x0D,
        kBackspace    = 0x0E,
        kTab          = 0x0F,
        kQ            = 0x10,
        kW            = 0x11,
        kE            = 0x12,
        kR            = 0x13,
        kT            = 0x14,
        kY            = 0x15,
        kU            = 0x16,
        kI            = 0x17,
        kO            = 0x18,
        kP            = 0x19,
        kBracketLeft  = 0x1A,
        kBracketRight = 0x1B,
        kEnter        = 0x1C,
        kLeftControl  = 0x1D,
        kA            = 0x1E,
        kS            = 0x1F,
        kD            = 0x20,
        kF            = 0x21,
        kG            = 0x22,
        kH            = 0x23,
        kJ            = 0x24,
        kK            = 0x25,
        kL            = 0x26,
        kSemicolon    = 0x27,
        kApostrophe   = 0x28,
        kTilde        = 0x29,
        kLeftShift    = 0x2A,
        kBackslash    = 0x2B,
        kZ            = 0x2C,
        kX            = 0x2D,
        kC            = 0x2E,
        kV            = 0x2F,
        kB            = 0x30,
        kN            = 0x31,
        kM            = 0x32,
        kComma        = 0x33,
        kPeriod       = 0x34,
        kSlash        = 0x35,
        kRightShift   = 0x36,
        kKP_Multiply  = 0x37,
        kLeftAlt      = 0x38,
        kSpacebar     = 0x39,
        kCapsLock     = 0x3A,
        kF1           = 0x3B,
        kF2           = 0x3C,
        kF3           = 0x3D,
        kF4           = 0x3E,
        kF5           = 0x3F,
        kF6           = 0x40,
        kF7           = 0x41,
        kF8           = 0x42,
        kF9           = 0x43,
        kF10          = 0x44,
        kNumLock      = 0x45,
        kScrollLock   = 0x46,
        kKP_7         = 0x47,
        kKP_8         = 0x48,
        kKP_9         = 0x49,
        kKP_Subtract  = 0x4A,
        kKP_4         = 0x4B,
        kKP_5         = 0x4C,
        kKP_6         = 0x4D,
        kKP_Plus      = 0x4E,
        kKP_1         = 0x4F,
        kKP_2         = 0x50,
        kKP_3         = 0x51,
        kKP_0         = 0x52,
        kKP_Decimal   = 0x53,
        kF11          = 0x57,
        kF12          = 0x58,
        kKP_Enter     = 0x9C,
        kRightControl = 0x9D,
        kKP_Divide    = 0xB5,
        kPrintScreen  = 0xB7,
        kRightAlt     = 0xB8,
        kPause        = 0xC5,
        kHome         = 0xC7,
        kUp           = 0xC8,
        kPageUp       = 0xC9,
        kLeft         = 0xCB,
        kRight        = 0xCD,
        kEnd          = 0xCF,
        kDown         = 0xD0,
        kPageDown     = 0xD1,
        kInsert       = 0xD2,
        kDelete       = 0xD3,
        kLeftWin      = 0xDB,
        kRightWin     = 0xDC,
    };
}

// XInput button masks used by Bethesda engine for gamepad
namespace GamepadKey {
    enum : std::uint32_t {
        kUp            = 0x0001,
        kDown          = 0x0002,
        kLeft          = 0x0004,
        kRight         = 0x0008,
        kStart         = 0x0010,
        kBack          = 0x0020,
        kLeftThumb     = 0x0040,
        kRightThumb    = 0x0080,
        kLeftShoulder  = 0x0100,
        kRightShoulder = 0x0200,
        kA             = 0x1000,
        kB             = 0x2000,
        kX             = 0x4000,
        kY             = 0x8000,
    };
}

// Mouse button codes as used by the Bethesda engine
namespace MouseKey {
    enum : std::uint32_t {
        kLeftButton   = 0,
        kRightButton  = 1,
        kMiddleButton = 2,
        kButton3      = 3,
        kButton4      = 4,
        kButton5      = 5,
        kButton6      = 6,
        kButton7      = 7,
        kWheelUp      = 8,
        kWheelDown    = 9,
    };
}

ImGuiKey ParseKeyFromKeyboard(std::uint32_t a_key) {
    switch (a_key) {
        case DIK::kTab:           return ImGuiKey_Tab;
        case DIK::kLeft:          return ImGuiKey_LeftArrow;
        case DIK::kRight:         return ImGuiKey_RightArrow;
        case DIK::kUp:            return ImGuiKey_UpArrow;
        case DIK::kDown:          return ImGuiKey_DownArrow;
        case DIK::kPageUp:        return ImGuiKey_PageUp;
        case DIK::kPageDown:      return ImGuiKey_PageDown;
        case DIK::kHome:          return ImGuiKey_Home;
        case DIK::kEnd:           return ImGuiKey_End;
        case DIK::kInsert:        return ImGuiKey_Insert;
        case DIK::kDelete:        return ImGuiKey_Delete;
        case DIK::kBackspace:     return ImGuiKey_Backspace;
        case DIK::kSpacebar:      return ImGuiKey_Space;
        case DIK::kEnter:         return ImGuiKey_Enter;
        case DIK::kEscape:        return ImGuiKey_Escape;
        case DIK::kLeftControl:   return ImGuiKey_ModCtrl;
        case DIK::kLeftShift:     return ImGuiKey_ModShift;
        case DIK::kLeftAlt:       return ImGuiKey_ModAlt;
        case DIK::kLeftWin:       return ImGuiKey_ModSuper;
        case DIK::kRightControl:  return ImGuiKey_ModCtrl;
        case DIK::kRightShift:    return ImGuiKey_ModShift;
        case DIK::kRightAlt:      return ImGuiKey_ModAlt;
        case DIK::kRightWin:      return ImGuiKey_ModSuper;
        case DIK::kNum0:          return ImGuiKey_0;
        case DIK::kNum1:          return ImGuiKey_1;
        case DIK::kNum2:          return ImGuiKey_2;
        case DIK::kNum3:          return ImGuiKey_3;
        case DIK::kNum4:          return ImGuiKey_4;
        case DIK::kNum5:          return ImGuiKey_5;
        case DIK::kNum6:          return ImGuiKey_6;
        case DIK::kNum7:          return ImGuiKey_7;
        case DIK::kNum8:          return ImGuiKey_8;
        case DIK::kNum9:          return ImGuiKey_9;
        case DIK::kA:             return ImGuiKey_A;
        case DIK::kB:             return ImGuiKey_B;
        case DIK::kC:             return ImGuiKey_C;
        case DIK::kD:             return ImGuiKey_D;
        case DIK::kE:             return ImGuiKey_E;
        case DIK::kF:             return ImGuiKey_F;
        case DIK::kG:             return ImGuiKey_G;
        case DIK::kH:             return ImGuiKey_H;
        case DIK::kI:             return ImGuiKey_I;
        case DIK::kJ:             return ImGuiKey_J;
        case DIK::kK:             return ImGuiKey_K;
        case DIK::kL:             return ImGuiKey_L;
        case DIK::kM:             return ImGuiKey_M;
        case DIK::kN:             return ImGuiKey_N;
        case DIK::kO:             return ImGuiKey_O;
        case DIK::kP:             return ImGuiKey_P;
        case DIK::kQ:             return ImGuiKey_Q;
        case DIK::kR:             return ImGuiKey_R;
        case DIK::kS:             return ImGuiKey_S;
        case DIK::kT:             return ImGuiKey_T;
        case DIK::kU:             return ImGuiKey_U;
        case DIK::kV:             return ImGuiKey_V;
        case DIK::kW:             return ImGuiKey_W;
        case DIK::kX:             return ImGuiKey_X;
        case DIK::kY:             return ImGuiKey_Y;
        case DIK::kZ:             return ImGuiKey_Z;
        case DIK::kF1:            return ImGuiKey_F1;
        case DIK::kF2:            return ImGuiKey_F2;
        case DIK::kF3:            return ImGuiKey_F3;
        case DIK::kF4:            return ImGuiKey_F4;
        case DIK::kF5:            return ImGuiKey_F5;
        case DIK::kF6:            return ImGuiKey_F6;
        case DIK::kF7:            return ImGuiKey_F7;
        case DIK::kF8:            return ImGuiKey_F8;
        case DIK::kF9:            return ImGuiKey_F9;
        case DIK::kF10:           return ImGuiKey_F10;
        case DIK::kF11:           return ImGuiKey_F11;
        case DIK::kF12:           return ImGuiKey_F12;
        case DIK::kApostrophe:    return ImGuiKey_Apostrophe;
        case DIK::kComma:         return ImGuiKey_Comma;
        case DIK::kMinus:         return ImGuiKey_Minus;
        case DIK::kPeriod:        return ImGuiKey_Period;
        case DIK::kSlash:         return ImGuiKey_Slash;
        case DIK::kSemicolon:     return ImGuiKey_Semicolon;
        case DIK::kEquals:        return ImGuiKey_Equal;
        case DIK::kBracketLeft:   return ImGuiKey_LeftBracket;
        case DIK::kBackslash:     return ImGuiKey_Backslash;
        case DIK::kBracketRight:  return ImGuiKey_RightBracket;
        case DIK::kTilde:         return ImGuiKey_GraveAccent;
        case DIK::kCapsLock:      return ImGuiKey_CapsLock;
        case DIK::kScrollLock:    return ImGuiKey_ScrollLock;
        case DIK::kNumLock:       return ImGuiKey_NumLock;
        case DIK::kPrintScreen:   return ImGuiKey_PrintScreen;
        case DIK::kPause:         return ImGuiKey_Pause;
        case DIK::kKP_0:          return ImGuiKey_Keypad0;
        case DIK::kKP_1:          return ImGuiKey_Keypad1;
        case DIK::kKP_2:          return ImGuiKey_Keypad2;
        case DIK::kKP_3:          return ImGuiKey_Keypad3;
        case DIK::kKP_4:          return ImGuiKey_Keypad4;
        case DIK::kKP_5:          return ImGuiKey_Keypad5;
        case DIK::kKP_6:          return ImGuiKey_Keypad6;
        case DIK::kKP_7:          return ImGuiKey_Keypad7;
        case DIK::kKP_8:          return ImGuiKey_Keypad8;
        case DIK::kKP_9:          return ImGuiKey_Keypad9;
        case DIK::kKP_Decimal:    return ImGuiKey_KeypadDecimal;
        case DIK::kKP_Divide:     return ImGuiKey_KeypadDivide;
        case DIK::kKP_Multiply:   return ImGuiKey_KeypadMultiply;
        case DIK::kKP_Subtract:   return ImGuiKey_KeypadSubtract;
        case DIK::kKP_Plus:       return ImGuiKey_KeypadAdd;
        case DIK::kKP_Enter:      return ImGuiKey_KeypadEnter;
        default:                  return ImGuiKey_None;
    }
}

inline ImGuiKey ParseKeyFromGamepad(std::uint32_t a_key) {
    switch (a_key) {
        case GamepadKey::kUp:            return ImGuiKey_GamepadDpadUp;
        case GamepadKey::kDown:          return ImGuiKey_GamepadDpadDown;
        case GamepadKey::kLeft:          return ImGuiKey_GamepadDpadLeft;
        case GamepadKey::kRight:         return ImGuiKey_GamepadDpadRight;
        case GamepadKey::kStart:         return ImGuiKey_GamepadStart;
        case GamepadKey::kBack:          return ImGuiKey_GamepadBack;
        case GamepadKey::kLeftThumb:     return ImGuiKey_GamepadL3;
        case GamepadKey::kRightThumb:    return ImGuiKey_GamepadR3;
        case GamepadKey::kLeftShoulder:  return ImGuiKey_GamepadL1;
        case GamepadKey::kRightShoulder: return ImGuiKey_GamepadR1;
        case GamepadKey::kA:             return ImGuiKey_GamepadFaceDown;
        case GamepadKey::kB:             return ImGuiKey_GamepadFaceRight;
        case GamepadKey::kX:             return ImGuiKey_GamepadFaceLeft;
        case GamepadKey::kY:             return ImGuiKey_GamepadFaceUp;
        default:                         return ImGuiKey_None;
    }
}

inline void TranslateButtonEvent(ImGuiIO& io, const RE::ButtonEvent* button) {
    if (!button->HasIDCode()) {
        return;
    }

    bool pressed = button->value != 0.0f;

    switch (*button->device) {
        case RE::INPUT_DEVICE::kKeyboard: {
            auto imKey = ParseKeyFromKeyboard(static_cast<std::uint32_t>(button->idCode));
            io.AddKeyEvent(imKey, pressed);
        } break;
        case RE::INPUT_DEVICE::kMouse: {
            auto key = static_cast<std::uint32_t>(button->idCode);
            switch (key) {
                case MouseKey::kWheelUp:
                    io.AddMouseWheelEvent(0, button->value);
                    break;
                case MouseKey::kWheelDown:
                    io.AddMouseWheelEvent(0, button->value * -1);
                    break;
                default:
                    io.AddMouseButtonEvent(key, pressed);
                    break;
            }
        } break;
        case RE::INPUT_DEVICE::kGamepad: {
            auto imKey = ParseKeyFromGamepad(static_cast<std::uint32_t>(button->idCode));
            io.AddKeyEvent(imKey, pressed);
        } break;
        default:
            break;
    }
}

void UI::TranslateInputEvent(RE::InputEvent* const* a_event) {
    auto& io = ImGui::GetIO();

    for (auto event = *a_event; event; event = event->next) {
        if (auto button = event->As<RE::ButtonEvent>()) {
            TranslateButtonEvent(io, button);
        } else if (auto charEvent = event->As<RE::CharacterEvent>()) {
            io.AddInputCharacter(charEvent->charCode);
        }
    }
}

void DoublePressDetector::press(){
    auto now = std::chrono::steady_clock::now();
    if (last_pressed_index) {
        last_pressed_times.second = now;
    } else {
        last_pressed_times.first = now;
    }
    increment();
};

DoublePressDetector::operator bool() const {
    const auto [first, second] = last_pressed_times;
    const int diff = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(second - first).count());
    return std::abs(diff) < double_press_threshold;
    
}
void DoublePressDetector::reset(){ last_pressed_times = {Timestamp::min(), Timestamp::min()};};
void DoublePressDetector::increment(){ last_pressed_index = !last_pressed_index; };

bool IsSupportedDevice(RE::INPUT_DEVICE device) {
    return device == RE::INPUT_DEVICE::kKeyboard || device == RE::INPUT_DEVICE::kGamepad;
}
