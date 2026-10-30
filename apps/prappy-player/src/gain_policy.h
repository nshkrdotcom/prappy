#pragma once
// Portable policy: no filesystem, libmpv, UI or audio-device dependencies.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gain {
enum class Origin { Disabled, R128, ReplayGain, Measurement, Fallback };
enum class TagState { Absent, Valid, Invalid, Unsupported };
struct Tag {
    Origin origin = Origin::Fallback;
    double adjustmentDb = 0;
    double referenceLufs = -18;
    std::optional<double> samplePeak;
    std::string reference;
};
struct Tags {
    TagState state = TagState::Absent;
    std::optional<Tag> selected;
    std::string reason;
};
struct Measurement {
    std::optional<double> loudness;
    std::optional<double> samplePeak;
};
struct Settings {
    bool enabled = false;
    double targetLufs = -18;
    double fallbackDb = 0;
};
struct Decision {
    uint64_t serial = 0;
    std::string source;
    bool enabled = false;
    double appliedDb = 0;
    Origin origin = Origin::Disabled;
    double targetLufs = -18;
    double referenceLufs = -18;
    bool peakLimited = false;
    bool unknownPeakCapped = false;
    std::optional<double> samplePeak;
    std::string reference;
};
// Called only with metadata already exposed by the loaded decoder.
Tags parseTags(const std::vector<std::pair<std::string, std::string>>& metadata,
               bool opusCodec);
Decision choose(uint64_t serial, std::string source, const Settings& settings,
                const Tags& tags, const Measurement& measurement);
const char* name(Origin source);
// The playback owner calls lockLoaded once for each matching loaded serial.
// Later measurements do not call this object. Only explicit settings actions
// call overrideSettings, and a rollback may deliberately reuse a prior value.
class Lock {
    std::optional<Decision> current_;
public:
    const std::optional<Decision>& current() const { return current_; }
    bool lockLoaded(Decision decision) {
        if (current_ && current_->serial == decision.serial) return false;
        current_ = std::move(decision); return true;
    }
    void overrideSettings(Decision decision) { current_ = std::move(decision); }
    std::optional<Decision> rollback(uint64_t serial, const std::string& source) const {
        if (!current_ || current_->source != source) return {};
        auto value = *current_; value.serial = serial; return value;
    }
};
}
