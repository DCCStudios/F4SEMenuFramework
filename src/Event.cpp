#include "Event.h"

int64_t autoIncrement = 0;

struct EventListener {
    Event::EventCallback callback;
    float priority;
};

static std::map<int64_t, EventListener> listeners;

int64_t Event::AddEventListener(EventCallback callback, float priority) {
    if (!callback) return 0;
    int64_t id = ++autoIncrement;
    listeners[id].callback = callback;
    return id;
}

void Event::RemoveEventListener(int64_t id) { listeners.erase(id); }

void Event::DispatchEvent(EventType type) {
    if (type == kNone) return;

    std::vector<EventListener*> ordered;
    ordered.reserve(listeners.size());

    for (auto& it : listeners) 
    {
        ordered.push_back(&it.second);
    }

    std::sort(ordered.begin(), ordered.end(),
              [](const EventListener* a, const EventListener* b) { return a->priority > b->priority; });

    for (auto* listener : ordered) 
    {
        if (listener->callback) listener->callback(type);
    }
}
