#pragma once

typedef bool(__stdcall* InputEventCallback)(RE::InputEvent*);

class InputEventHandler {
    static inline std::map<uint64_t, InputEventCallback> callbacks;
    static bool InputEvent(RE::InputEvent* event);
    static inline uint64_t auto_increment = 0;
public:
    // Which subset of the queue a Process() pass should dispatch. The queue is
    // dispatched from two receivers (see Hooks::InputQueueHook) because no
    // single one carries every device in every game state: PlayerCamera+0x38
    // has keyboard in all states but loses the mouse wheel while the player is
    // sighted; PlayerControls has the mouse (and gamepad) in all states but no
    // keyboard. Splitting by device across the two receivers gives full
    // coverage with each event dispatched exactly once.
    enum class DeviceFilter {
        kAll,         // every device (single-receiver use)
        kNonPointer,  // keyboard + anything that is not mouse/gamepad
        kPointer      // mouse + gamepad
    };

    // Dispatches the matching subset of the queue to registered callbacks,
    // unlinking consumed events (events outside the filter are left untouched
    // and forwarded to the engine). Runs on the game's input thread.
    static RE::InputEvent* const* Process(RE::InputEvent** a_event, DeviceFilter filter = DeviceFilter::kAll);
    static bool HasCallbacks();
    static int64_t Register(InputEventCallback callback);
    static void Unregister(uint64_t id);
};
