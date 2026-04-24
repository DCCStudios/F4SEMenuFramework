#pragma once

typedef bool(__stdcall* InputEventCallback)(RE::InputEvent*);

class InputEventHandler {
    static inline std::map<uint64_t, InputEventCallback> callbacks;
    static inline bool middleButtonDown = false;
    static bool InputEvent(RE::InputEvent* event);
    static inline uint64_t auto_increment = 0;
public:
    static RE::InputEvent* const* Process(RE::InputEvent** a_event);
    static int64_t Register(InputEventCallback callback);
    static void Unregister(uint64_t id);
};
