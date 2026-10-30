#include "gain_policy.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>

namespace gain {
namespace {
std::string lower(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
std::string trim(std::string_view text) {
    auto l = text.find_first_not_of(" \t\r\n");
    if (l == text.npos) return {};
    return std::string(text.substr(l, text.find_last_not_of(" \t\r\n") - l + 1));
}
std::optional<double> decimal(std::string_view text, bool dbSuffix = false) {
    auto value = trim(text);
    if (dbSuffix && value.size() >= 2 && lower(value.substr(value.size() - 2)) == "db")
        value = trim(std::string_view(value).substr(0, value.size() - 2));
    if (value.empty()) return {};
    double number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number,
                                              std::chars_format::general);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number)) return {};
    return number;
}
std::optional<int> r128(std::string_view text) {
    const auto value = trim(text);
    if (value.empty()) return {};
    int number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() || number < -32768 || number > 32767) return {};
    return number;
}
}
Tags parseTags(const std::vector<std::pair<std::string, std::string>>& metadata, bool opusCodec) {
    std::map<std::string, std::string> keys;
    bool conflict = false;
    for (const auto& [key, value] : metadata) {
        auto normalized = lower(trim(key));
        const auto content = trim(value);
        const auto [it, added] = keys.emplace(normalized, content);
        if (!added && it->second != content) conflict = true;
    }
    const auto get = [&](std::string_view key) -> std::optional<std::string> {
        if (auto it = keys.find(std::string(key)); it != keys.end()) return it->second;
        return {};
    };
    Tags output;
    if (conflict) { output.state = TagState::Invalid; output.reason = "Conflicting duplicate metadata keys"; return output; }
    if (opusCodec) {
        if (auto raw = get("r128_track_gain")) {
            const auto value = r128(*raw);
            if (!value) { output.state = TagState::Invalid; output.reason = "Invalid Opus R128 Q7.8 gain"; return output; }
            // RFC 7845: the Opus header output gain is *already* applied by
            // the decoder; this comment field is an additional adjustment
            // with -23 LUFS reference. Never add header gain a second time.
            output.state = TagState::Valid;
            output.selected = Tag{Origin::R128, *value / 256.0, -23, {}, "Opus R128 Q7.8, -23 LUFS"};
            return output;
        }
    }
    const auto rg = get("replaygain_track_gain");
    const auto peak = get("replaygain_track_peak");
    const auto ref = get("replaygain_reference_loudness");
    if (rg) {
        const auto adjustment = decimal(*rg, true);
        const auto p = peak ? decimal(*peak) : std::optional<double>{};
        if (!adjustment || *adjustment < -96 || *adjustment > 96 ||
            (peak && (!p || *p <= 0 || *p > 64))) {
            output.state = TagState::Invalid; output.reason = "Invalid ReplayGain track gain/peak"; return output;
        }
        // ReplayGain 89 dB SPL is not a normative LUFS calibration. We accept
        // only its explicitly identified standard reference and document the
        // application's -18 LUFS alignment assumption. Never treat a bare
        // numeric gain as an integrated LUFS measurement.
        if (ref) {
            auto number = decimal(*ref, true);
            if (!number || std::abs(*number - 89) > .0001) {
                output.state = TagState::Unsupported;
                output.reason = "Unknown ReplayGain reference (expected 89 dB)"; return output;
            }
        }
        output.state = TagState::Valid;
        output.selected = Tag{Origin::ReplayGain, *adjustment, -18, opusCodec ? std::optional<double>{} : p,
            "ReplayGain 89 dB SPL (application -18 LUFS alignment assumption)"};
        return output;
    }
    if (get("itunnorm") || get("com.apple.itunes:itunnorm") || get("itunnorm:eng")) {
        output.state = TagState::Unsupported;
        output.reason = "iTunNORM conversion not yet verified against Sound Check fixtures";
        return output;
    }
    if (get("r128_track_gain") || peak || ref || get("replaygain_album_gain") || get("r128_album_gain")) {
        output.state = TagState::Unsupported;
        output.reason = "No supported track gain/reference";
    }
    return output;
}
Decision choose(uint64_t serial, std::string source, const Settings& settings, const Tags& tags,
                const Measurement& measurement) {
    Decision result;
    result.serial = serial; result.source = std::move(source); result.enabled = settings.enabled;
    result.targetLufs = std::isfinite(settings.targetLufs) ? settings.targetLufs : -18;
    if (!settings.enabled) return result;
    double requested = std::isfinite(settings.fallbackDb) ? settings.fallbackDb : 0;
    result.origin = Origin::Fallback;
    // Peak measurements are from the same 48 kHz stereo PCM domain as mpv
    // playback, including mandatory codec output gain. A valid newer measured
    // peak is preferred over a potentially pre-decoder tag peak.
    if (measurement.samplePeak && std::isfinite(*measurement.samplePeak) && *measurement.samplePeak > 0 && *measurement.samplePeak <= 64)
        result.samplePeak = measurement.samplePeak;
    if (tags.selected) {
        const auto& tag = *tags.selected;
        if (std::isfinite(tag.adjustmentDb) && std::isfinite(tag.referenceLufs)) {
            result.origin = tag.origin; result.reference = tag.reference;
            result.referenceLufs = tag.referenceLufs;
            requested = tag.adjustmentDb + result.targetLufs - tag.referenceLufs;
            if (!result.samplePeak && tag.samplePeak && std::isfinite(*tag.samplePeak) && *tag.samplePeak > 0 && *tag.samplePeak <= 64)
                result.samplePeak = tag.samplePeak;
        }
    } else if (measurement.loudness && std::isfinite(*measurement.loudness) &&
               *measurement.loudness >= -70 && *measurement.loudness <= 50) {
        result.origin = Origin::Measurement;
        result.referenceLufs = *measurement.loudness;
        result.reference = "ITU-R BS.1770 measured integrated LUFS";
        requested = result.targetLufs - *measurement.loudness;
    }
    requested = std::clamp(requested, -96.0, 12.0);
    if (result.samplePeak) {
        const auto limit = -1.0 - 20 * std::log10(*result.samplePeak);
        result.peakLimited = requested > limit;
        requested = std::min(requested, limit);
    } else if (requested > 0) {
        result.unknownPeakCapped = true;
        requested = 0;
    }
    result.appliedDb = std::clamp(requested, -96.0, 12.0);
    return result;
}
const char* name(Origin source) {
    switch (source) {
    case Origin::Disabled: return "disabled";
    case Origin::R128: return "Opus R128 tag";
    case Origin::ReplayGain: return "ReplayGain track tag";
    case Origin::Measurement: return "measured LUFS";
    case Origin::Fallback: return "unmeasured fallback";
    }
    return "unknown";
}
}
