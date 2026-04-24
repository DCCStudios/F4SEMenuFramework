#include "InputEventHandler.h"
bool InputEventHandler::InputEvent(RE::InputEvent* event) {

    bool result = false;

    for (auto item : callbacks) {
        if (item.second(event)) {
            result = true;
        }
    }

    return result;
}

RE::InputEvent* const* InputEventHandler::Process(RE::InputEvent** a_event) { 
    auto first = *a_event;
    auto last = *a_event;
    size_t length = 0;

    for (auto current = *a_event; current; current = current->next) {
        if (InputEvent(current)) {
            if (current != last) {
                last->next = current->next;
            } else {
                last = current->next;
                first = current->next;
            }
        } else {
            last = current;
            ++length;
        }
    }
    a_event[0] = first;
    return a_event;
}


int64_t InputEventHandler::Register(InputEventCallback callback) { 
    auto result = auto_increment++;
    callbacks[result] = callback;
    return result;
}

void InputEventHandler::Unregister(uint64_t id) { 
    auto it = callbacks.find(id);
    if (it != callbacks.end()) {
        callbacks.erase(it); 
    }
}

