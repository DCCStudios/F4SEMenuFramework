#pragma once


namespace GameLock {
    enum State { None, Locked, Unlocked, Resume };
    extern State lastState;
    void SetState(State currentState);
}
