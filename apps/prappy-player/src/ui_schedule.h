#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

struct UiSchedule {
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    bool dirty = true;
    unsigned settle = 3;
    Time paint = Clock::now(), hover = Time::max(), caret = Time::max();
    uint64_t presents = 0, wakeups = 0;
    void request(unsigned frames = 1) { dirty = true; settle = std::max(settle, frames); }
    void input(Time now) { request(3); paint = std::min(paint,now); hover = now + std::chrono::milliseconds(200); }
    int wait(Time now, bool visible, bool animated, bool testing, bool scanning, Time persistence) const {
        auto deadline = now + std::chrono::seconds(1); // Lost-notification watchdog; never forces painting.
        if (visible && (dirty || settle || animated)) deadline = std::min(deadline, paint);
        if (visible) { deadline = std::min(deadline, hover); deadline = std::min(deadline, caret); }
        if (testing) deadline = std::min(deadline, now + std::chrono::milliseconds(16));
        if (scanning) deadline = std::min(deadline, now + std::chrono::milliseconds(250));
        if (persistence > Time{} && persistence != Time::max()) deadline = std::min(deadline, persistence);
        if (animated && !visible) deadline = std::min(deadline, paint);
        return static_cast<int>(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 0, 1000));
    }
    bool due(Time now, bool animated) {
        if (now >= hover) { hover = Time::max(); request(2); }
        if (now >= caret) { caret = Time::max(); request(); }
        return now >= paint && (dirty || settle || animated);
    }
    void presented(Time now) {
        dirty = false; if (settle) --settle;
        paint = now + std::chrono::milliseconds(16); ++presents;
    }
};
inline double approachGain(double current, double target, double seconds) {
    return current + std::clamp(target - current, -9 * seconds, 9 * seconds);
}
inline double releasePeak(double previous, double signal, double seconds) {
    const auto value = std::max(signal, previous * std::exp(-seconds / .258));
    return value < .00001 ? 0 : value;
}
