#pragma once
typedef void(__stdcall* HudElementCallback)();

class HudManager {
    static inline uint64_t auto_increment = 0;
    static inline std::map<uint64_t, HudElementCallback> callbacks;

public:
    static void Render();
    static int64_t Register(HudElementCallback callback);
    static void Unregister(uint64_t id);
};