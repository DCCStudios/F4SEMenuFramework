#pragma once
namespace Event {

    enum EventType {
		kNone = 0,
		kOpenMenu = 1,
		kCloseMenu = 2,
		kBeforeRender = 3,
		kAfterRender = 4
	};

    typedef void(__stdcall* EventCallback)(EventType type);

    int64_t AddEventListener(EventCallback callback, float priority);
    void RemoveEventListener(int64_t id);
    void DispatchEvent(EventType type);
}