#include "InputEventHandler.h"

#include <mutex>
#include <vector>

// Guards `callbacks`: Register/Unregister are exported API surface a plugin
// can call from any thread at any time, while Process runs on the game's
// input thread every frame. Process copies the callbacks under the lock and
// dispatches from the snapshot, so a callback may itself Register/Unregister
// (including unregistering itself) without deadlock or iterator invalidation.
static std::mutex s_callbackMutex;

bool InputEventHandler::InputEvent(RE::InputEvent* event) {
    std::vector<InputEventCallback> snapshot;
    {
        std::lock_guard lock(s_callbackMutex);
        snapshot.reserve(callbacks.size());
        for (const auto& item : callbacks) {
            snapshot.push_back(item.second);
        }
    }

    bool result = false;
    for (auto callback : snapshot) {
        if (callback(event)) {
            result = true;
        }
    }
    return result;
}

bool InputEventHandler::HasCallbacks() {
    std::lock_guard lock(s_callbackMutex);
    return !callbacks.empty();
}

// True when `event` belongs to the subset this pass should handle. Events
// outside the filter are skipped entirely (never dispatched, never unlinked),
// so the other receiver's pass owns them and no event is dispatched twice.
static bool MatchesFilter(RE::InputEvent* event, InputEventHandler::DeviceFilter filter) {
    if (filter == InputEventHandler::DeviceFilter::kAll) return true;
    const auto device = *event->device;
    const bool isPointer = device == RE::INPUT_DEVICE::kMouse ||
                           device == RE::INPUT_DEVICE::kGamepad;
    return filter == InputEventHandler::DeviceFilter::kPointer ? isPointer : !isPointer;
}

// Dispatches the matching events in the queue to the registered callbacks and
// unlinks the ones a callback consumed (returned true for), so the caller can
// forward a filtered chain to the game. Non-matching events are left linked so
// the other receiver's pass can dispatch them and the engine still sees them.
// Returns the possibly-new queue head via the in/out pointer; *a_event becomes
// nullptr when everything was consumed.
RE::InputEvent* const* InputEventHandler::Process(RE::InputEvent** a_event, DeviceFilter filter) {
    auto first = *a_event;
    auto last = *a_event;

    for (auto current = *a_event; current; current = current->next) {
        // Only dispatch (and thus only ever unlink) events in this filter's
        // device class; everything else stays in the chain untouched.
        if (MatchesFilter(current, filter) && InputEvent(current)) {
            if (current != last) {
                last->next = current->next;
            } else {
                last = current->next;
                first = current->next;
            }
        } else {
            last = current;
        }
    }
    a_event[0] = first;
    return a_event;
}


int64_t InputEventHandler::Register(InputEventCallback callback) {
    std::lock_guard lock(s_callbackMutex);
    auto result = auto_increment++;
    callbacks[result] = callback;
    return result;
}

void InputEventHandler::Unregister(uint64_t id) {
    std::lock_guard lock(s_callbackMutex);
    auto it = callbacks.find(id);
    if (it != callbacks.end()) {
        callbacks.erase(it);
    }
}
