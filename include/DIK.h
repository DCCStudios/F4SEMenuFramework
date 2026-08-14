#pragma once

// ---------------------------------------------------------------------------
// Canonical input-code tables.
//
// ONE list per device drives everything that used to be written out by hand:
//   * the named constants below — what RegisterHotkey / SetHotkeyBinding take
//     as `defaultScanCode`, so callers write DIK::F1 instead of 0x3B
//   * GetKeyBinding()  name -> code   (INI parsing)
//   * GetKeyName()     code -> name   (INI writing, UI labels, MCM export)
//
// Before this, GetKeyBinding and GetKeyName each carried their own literal
// map of the same ~110 entries and could silently disagree — a key added to
// one but not the other parses from the INI and then saves back as UNKNOWN.
// Add a key here once and every consumer picks it up.
//
// The INI name strings are load-bearing: they are what users already have in
// F4SEMenuFramework.ini and in MCM keybind exports. Change an identifier
// freely; changing the middle column breaks existing configs.
//
// Identifiers are mixed-case on purpose. Windows headers #define several
// all-caps names we'd otherwise collide with — DELETE is (0x00010000L) in
// winnt.h, which would make `DIK::DELETE` fail to compile.
// ---------------------------------------------------------------------------

// Keyboard: DirectInput (DIK_*) scan codes, as BSInputDevice reports them.
// These are NOT Windows virtual-key codes. Extended keys carry bit 0x80
// (PageUp is 0xC9, not 0x49) — see wmLParamToDIK() in Hooks.cpp for the
// WM_KEYDOWN conversion that restores that bit.
//
//                identifier        INI name          code
#define F4SEMF_DIK_LIST(F4SEMF_X)                              \
    F4SEMF_X(None,            "NONE",           0x00)          \
    F4SEMF_X(Escape,          "ESCAPE",         0x01)          \
    F4SEMF_X(Num1,            "1",              0x02)          \
    F4SEMF_X(Num2,            "2",              0x03)          \
    F4SEMF_X(Num3,            "3",              0x04)          \
    F4SEMF_X(Num4,            "4",              0x05)          \
    F4SEMF_X(Num5,            "5",              0x06)          \
    F4SEMF_X(Num6,            "6",              0x07)          \
    F4SEMF_X(Num7,            "7",              0x08)          \
    F4SEMF_X(Num8,            "8",              0x09)          \
    F4SEMF_X(Num9,            "9",              0x0A)          \
    F4SEMF_X(Num0,            "0",              0x0B)          \
    F4SEMF_X(Minus,           "MINUS",          0x0C)          \
    F4SEMF_X(Equals,          "EQUALS",         0x0D)          \
    F4SEMF_X(Backspace,       "BACKSPACE",      0x0E)          \
    F4SEMF_X(Tab,             "TAB",            0x0F)          \
    F4SEMF_X(Q,               "Q",              0x10)          \
    F4SEMF_X(W,               "W",              0x11)          \
    F4SEMF_X(E,               "E",              0x12)          \
    F4SEMF_X(R,               "R",              0x13)          \
    F4SEMF_X(T,               "T",              0x14)          \
    F4SEMF_X(Y,               "Y",              0x15)          \
    F4SEMF_X(U,               "U",              0x16)          \
    F4SEMF_X(I,               "I",              0x17)          \
    F4SEMF_X(O,               "O",              0x18)          \
    F4SEMF_X(P,               "P",              0x19)          \
    F4SEMF_X(BracketLeft,     "BRACKETLEFT",    0x1A)          \
    F4SEMF_X(BracketRight,    "BRACKETRIGHT",   0x1B)          \
    F4SEMF_X(Enter,           "ENTER",          0x1C)          \
    F4SEMF_X(LeftControl,     "LEFTCONTROL",    0x1D)          \
    F4SEMF_X(A,               "A",              0x1E)          \
    F4SEMF_X(S,               "S",              0x1F)          \
    F4SEMF_X(D,               "D",              0x20)          \
    F4SEMF_X(F,               "F",              0x21)          \
    F4SEMF_X(G,               "G",              0x22)          \
    F4SEMF_X(H,               "H",              0x23)          \
    F4SEMF_X(J,               "J",              0x24)          \
    F4SEMF_X(K,               "K",              0x25)          \
    F4SEMF_X(L,               "L",              0x26)          \
    F4SEMF_X(Semicolon,       "SEMICOLON",      0x27)          \
    F4SEMF_X(Apostrophe,      "APOSTROPHE",     0x28)          \
    F4SEMF_X(Tilde,           "TILDE",          0x29)          \
    F4SEMF_X(LeftShift,       "LEFTSHIFT",      0x2A)          \
    F4SEMF_X(Backslash,       "BACKSLASH",      0x2B)          \
    F4SEMF_X(Z,               "Z",              0x2C)          \
    F4SEMF_X(X,               "X",              0x2D)          \
    F4SEMF_X(C,               "C",              0x2E)          \
    F4SEMF_X(V,               "V",              0x2F)          \
    F4SEMF_X(B,               "B",              0x30)          \
    F4SEMF_X(N,               "N",              0x31)          \
    F4SEMF_X(M,               "M",              0x32)          \
    F4SEMF_X(Comma,           "COMMA",          0x33)          \
    F4SEMF_X(Period,          "PERIOD",         0x34)          \
    F4SEMF_X(Slash,           "SLASH",          0x35)          \
    F4SEMF_X(RightShift,      "RIGHTSHIFT",     0x36)          \
    F4SEMF_X(NumpadMultiply,  "KP_MULTIPLY",    0x37)          \
    F4SEMF_X(LeftAlt,         "LEFTALT",        0x38)          \
    F4SEMF_X(Spacebar,        "SPACEBAR",       0x39)          \
    F4SEMF_X(CapsLock,        "CAPSLOCK",       0x3A)          \
    F4SEMF_X(F1,              "F1",             0x3B)          \
    F4SEMF_X(F2,              "F2",             0x3C)          \
    F4SEMF_X(F3,              "F3",             0x3D)          \
    F4SEMF_X(F4,              "F4",             0x3E)          \
    F4SEMF_X(F5,              "F5",             0x3F)          \
    F4SEMF_X(F6,              "F6",             0x40)          \
    F4SEMF_X(F7,              "F7",             0x41)          \
    F4SEMF_X(F8,              "F8",             0x42)          \
    F4SEMF_X(F9,              "F9",             0x43)          \
    F4SEMF_X(F10,             "F10",            0x44)          \
    F4SEMF_X(NumLock,         "NUMLOCK",        0x45)          \
    F4SEMF_X(ScrollLock,      "SCROLLLOCK",     0x46)          \
    F4SEMF_X(Numpad7,         "KP_7",           0x47)          \
    F4SEMF_X(Numpad8,         "KP_8",           0x48)          \
    F4SEMF_X(Numpad9,         "KP_9",           0x49)          \
    F4SEMF_X(NumpadSubtract,  "KP_SUBTRACT",    0x4A)          \
    F4SEMF_X(Numpad4,         "KP_4",           0x4B)          \
    F4SEMF_X(Numpad5,         "KP_5",           0x4C)          \
    F4SEMF_X(Numpad6,         "KP_6",           0x4D)          \
    F4SEMF_X(NumpadPlus,      "KP_PLUS",        0x4E)          \
    F4SEMF_X(Numpad1,         "KP_1",           0x4F)          \
    F4SEMF_X(Numpad2,         "KP_2",           0x50)          \
    F4SEMF_X(Numpad3,         "KP_3",           0x51)          \
    F4SEMF_X(Numpad0,         "KP_0",           0x52)          \
    F4SEMF_X(NumpadDecimal,   "KP_DECIMAL",     0x53)          \
    F4SEMF_X(F11,             "F11",            0x57)          \
    F4SEMF_X(F12,             "F12",            0x58)          \
    F4SEMF_X(NumpadEnter,     "KP_ENTER",       0x9C)          \
    F4SEMF_X(RightControl,    "RIGHTCONTROL",   0x9D)          \
    F4SEMF_X(NumpadDivide,    "KP_DIVIDE",      0xB5)          \
    F4SEMF_X(PrintScreen,     "PRINTSCREEN",    0xB7)          \
    F4SEMF_X(RightAlt,        "RIGHTALT",       0xB8)          \
    F4SEMF_X(Pause,           "PAUSE",          0xC5)          \
    F4SEMF_X(Home,            "HOME",           0xC7)          \
    F4SEMF_X(Up,              "UP",             0xC8)          \
    F4SEMF_X(PageUp,          "PAGEUP",         0xC9)          \
    F4SEMF_X(Left,            "LEFT",           0xCB)          \
    F4SEMF_X(Right,           "RIGHT",          0xCD)          \
    F4SEMF_X(End,             "END",            0xCF)          \
    F4SEMF_X(Down,            "DOWN",           0xD0)          \
    F4SEMF_X(PageDown,        "PAGEDOWN",       0xD1)          \
    F4SEMF_X(Insert,          "INSERT",         0xD2)          \
    F4SEMF_X(Delete,          "DELETE",         0xD3)          \
    F4SEMF_X(LeftWin,         "LEFTWIN",        0xDB)          \
    F4SEMF_X(RightWin,        "RIGHTWIN",       0xDC)          \
    /* Mouse buttons sit above the 8-bit DIK range, following the       */ \
    /* F4SE/Papyrus keycode convention (256 = left). The real MCM binds  */ \
    /* mouse buttons, so the translation layer has to carry them.        */ \
    F4SEMF_X(MouseLeft,       "MOUSELEFT",      256)           \
    F4SEMF_X(MouseRight,      "MOUSERIGHT",     257)           \
    F4SEMF_X(MouseMiddle,     "MOUSEMIDDLE",    258)           \
    F4SEMF_X(Mouse4,          "MOUSE4",         259)           \
    F4SEMF_X(Mouse5,          "MOUSE5",         260)

// Gamepad: framework config codes, NOT raw XINPUT_GAMEPAD_* masks.
//
// Most are the XInput bitmask, but the triggers are not — they are analog and
// have no bit in wButtons, so 9 and 10 are sentinels that sit in the gap
// between DpadRight (8) and Start (16). GamepadInput::ConfigCodeToXInputMask
// returns 0 for both and IsToggleButtonDown() special-cases them against
// TRIGGER_THRESHOLD. Do not "fix" them into powers of two.
//
//                    identifier    INI name      code
#define F4SEMF_GAMEPAD_LIST(F4SEMF_X)                          \
    F4SEMF_X(None,            "NONE",           0)             \
    F4SEMF_X(DpadUp,          "DPAD_UP",        1)             \
    F4SEMF_X(DpadDown,        "DPAD_DOWN",      2)             \
    F4SEMF_X(DpadLeft,        "DPAD_LEFT",      4)             \
    F4SEMF_X(DpadRight,       "DPAD_RIGHT",     8)             \
    F4SEMF_X(LeftTrigger,     "LT",             9)             \
    F4SEMF_X(RightTrigger,    "RT",             10)            \
    F4SEMF_X(Start,           "START",          16)            \
    F4SEMF_X(Back,            "BACK",           32)            \
    F4SEMF_X(LeftStick,       "LS",             64)            \
    F4SEMF_X(RightStick,      "RS",             128)           \
    F4SEMF_X(LeftBumper,      "LB",             256)           \
    F4SEMF_X(RightBumper,     "RB",             512)           \
    F4SEMF_X(A,               "A",              4096)          \
    F4SEMF_X(B,               "B",              8192)          \
    F4SEMF_X(X,               "X",              16384)         \
    F4SEMF_X(Y,               "Y",              32768)

// --- Named constants -------------------------------------------------------
// DIK::F1, GamepadCode::DpadUp, ... Pass these to RegisterHotkey /
// RegisterGamepadHotkey / SetHotkeyBinding instead of raw numbers.
//
// The two namespaces overlap numerically (DIK::MouseLeft and
// GamepadCode::LeftBumper are both 256) — codes are only meaningful together
// with their RE::INPUT_DEVICE, which is why they are separate namespaces
// rather than one flat enum.

namespace DIK {
#define F4SEMF_DIK_CONSTANT(ident, name, code) inline constexpr unsigned int ident = code;
    F4SEMF_DIK_LIST(F4SEMF_DIK_CONSTANT)
#undef F4SEMF_DIK_CONSTANT

    // True if `code` is a value this table actually defines. The framework
    // warns when a hotkey is registered with an unknown code, since the usual
    // cause is passing a Windows virtual-key (VK_*) value by mistake — VK_F1
    // is 0x70, which is not a DIK scan code. Not a perfect catch: VK values
    // that happen to alias a real DIK code (VK 'A' 0x41 == DIK::F7) pass
    // through. Also usable by plugin rebind UIs to validate captured input.
    [[nodiscard]] constexpr bool IsKnownCode(unsigned int code) noexcept {
        switch (code) {
#define F4SEMF_DIK_CASE(ident, name, val) case (val):
            F4SEMF_DIK_LIST(F4SEMF_DIK_CASE)
#undef F4SEMF_DIK_CASE
            return true;
        default:
            return false;
        }
    }
}

// Modifier keys for chord hotkeys (e.g. Ctrl+Shift+F1). A bitmask combined
// with a scan code via the *WithModifiers hotkey entry points. These are
// keyboard modifiers read from the physical Ctrl/Shift/Alt keys — for a
// gamepad hotkey they mean "this button while holding that keyboard modifier".
// A value of 0 (None) is a plain, unmodified binding: every hotkey registered
// through the original entry points is None, and plain bindings keep firing
// regardless of which modifiers are held, so adding this never changes them.
namespace HotkeyMod {
    inline constexpr unsigned int None  = 0;
    inline constexpr unsigned int Ctrl  = 1u << 0;
    inline constexpr unsigned int Shift = 1u << 1;
    inline constexpr unsigned int Alt   = 1u << 2;
}

namespace GamepadCode {
#define F4SEMF_DIK_CONSTANT(ident, name, code) inline constexpr unsigned int ident = code;
    F4SEMF_GAMEPAD_LIST(F4SEMF_DIK_CONSTANT)
#undef F4SEMF_DIK_CONSTANT

    // True if `code` is a value this table defines (a framework gamepad config
    // code, not a raw XINPUT_GAMEPAD_* mask — the triggers 9/10 are the ones
    // that differ). Used to warn on likely-wrong RegisterGamepadHotkey codes.
    [[nodiscard]] constexpr bool IsKnownCode(unsigned int code) noexcept {
        switch (code) {
#define F4SEMF_GP_CASE(ident, name, val) case (val):
            F4SEMF_GAMEPAD_LIST(F4SEMF_GP_CASE)
#undef F4SEMF_GP_CASE
            return true;
        default:
            return false;
        }
    }
}
