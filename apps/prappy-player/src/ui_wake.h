#pragma once
#include <SDL3/SDL.h>
#include <atomic>
#include <stdexcept>

// Shared by callbacks; no window pointers, no application state in SDL events.
// Clear before draining mailboxes so a concurrent publisher can post again.
class UiWake {
    Uint32 type;
    std::atomic<bool> posted{false};
    std::atomic<unsigned> reasons{0};
public:
    enum Reason : unsigned { Work = 1, Audio = 2 };
    UiWake() : type(SDL_RegisterEvents(1)) { if (!type) throw std::runtime_error("Cannot register UI wake event."); }
    Uint32 eventType() const { return type; }
    void notify(unsigned reason = Work) noexcept {
        reasons.fetch_or(reason, std::memory_order_release);
        if (!posted.exchange(true, std::memory_order_acq_rel)) {
            SDL_Event event{}; event.type = type;
            if (!SDL_PushEvent(&event)) posted.store(false, std::memory_order_release);
        }
    }
    unsigned acknowledge() { posted.store(false, std::memory_order_release); return reasons.exchange(0, std::memory_order_acq_rel); }
};
