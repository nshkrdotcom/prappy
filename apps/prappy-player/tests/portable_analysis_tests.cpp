#include "gain_policy.h"


#include "analysis_dsp.h"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
static void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
static void rejects(auto action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
static void gainTests() {
    gain::Settings enabled{true, -18, 0};
    auto noTag = gain::parseTags({}, false);
    check(noTag.state == gain::TagState::Absent, "No-tag metadata");
    auto unknown = gain::choose(10, "song-a", enabled, noTag, {});
    check(unknown.appliedDb == 0 && unknown.origin == gain::Origin::Fallback, "Fallback gain");
    auto measured = gain::choose(10, "song-a", enabled, noTag, {-25, 0.1});
    check(measured.origin == gain::Origin::Measurement && std::abs(measured.appliedDb - 7) < 1e-9,
          "Measured gain");
    check(gain::choose(11, "a", enabled, noTag, {-30, {}}).appliedDb == 0, "Unknown peak may not boost");
    check(gain::choose(11, "a", enabled, noTag, {-30, 1}).appliedDb == -1, "Peak cap");
    check(gain::choose(11, "a", enabled, noTag, {-60, 0.01}).appliedDb == 12, "Maximum boost");
    check(gain::choose(11, "a", enabled, noTag, {-12, 0.1}).appliedDb == -6, "Attenuation");
    auto disabled = enabled; disabled.enabled = false;
    check(gain::choose(11, "a", disabled, noTag, {-30, 1}).appliedDb == 0, "Disabled normalization");
    auto r128 = gain::parseTags({{"R128_TRACK_GAIN", "-512"}, {"REPLAYGAIN_TRACK_GAIN", "10 dB"}}, true);
    check(r128.selected && r128.selected->origin == gain::Origin::R128, "Opus precedence");
    const auto r128Result = gain::choose(12, "opus", enabled, r128, {-27, 0.1});
    check(std::abs(r128Result.appliedDb - 3) < 1e-9, "Opus -23 LUFS conversion");
    check(std::abs(r128Result.referenceLufs + 23) < 1e-9, "Opus reference");
    auto rg = gain::parseTags({{"ReplayGain_Track_Gain", "-3.50 dB"},
                               {"REPLAYGAIN_REFERENCE_LOUDNESS", "89 dB"},
                               {"replaygain_track_peak", "0.5"}}, false);
    check(rg.selected && rg.selected->origin == gain::Origin::ReplayGain, "ReplayGain parsing");
    check(std::abs(gain::choose(13,"flac",enabled,rg,{}).appliedDb + 3.5) < 1e-9, "ReplayGain target");
    enabled.targetLufs = -16;
    check(std::abs(gain::choose(13,"flac",enabled,rg,{}).appliedDb + 1.5) < 1e-9, "Target conversion");
    check(gain::parseTags({{"replaygain_track_gain", "NaN"}}, false).state == gain::TagState::Invalid,
          "Nonfinite ReplayGain");
    check(gain::parseTags({{"replaygain_track_gain", "5db"}, {"REPLAYGAIN_TRACK_GAIN", "4db"}}, false).state == gain::TagState::Invalid,
          "Conflicting duplicate tag");
    check(gain::parseTags({{"replaygain_track_gain", "5 dB"}, {"replaygain_reference_loudness", "83 dB"}}, false).state == gain::TagState::Unsupported,
          "Unknown ReplayGain reference");
    check(gain::parseTags({{"r128_track_gain", "32768"}}, true).state == gain::TagState::Invalid, "R128 range");
    const auto positiveR128 = gain::parseTags({{"R128_TRACK_GAIN", "+512"}}, true);
    check(positiveR128.selected && positiveR128.selected->adjustmentDb == 2, "Signed positive R128 was rejected");
    for (const auto* invalid : {" 512", "512 ", "0000512", "+-512"})
        check(gain::parseTags({{"R128_TRACK_GAIN", invalid}}, true).state == gain::TagState::Invalid,
              "R128 accepted whitespace, excess digits or multiple signs");
    check(gain::parseTags({{"R128_TRACK_GAIN", "512"}, {"R128_TRACK_GAIN", "512"}}, true).state == gain::TagState::Invalid,
          "R128 accepted duplicate track gain tags");
    check(gain::parseTags({{"iTunNORM", "000016E7 000016E7 ..."}}, false).state == gain::TagState::Unsupported,
          "Sound Check must not silently invent a conversion");
    check(gain::parseTags({{"r128_track_gain", "200"}}, false).state == gain::TagState::Unsupported,
          "Non-Opus R128 reference must not be assumed");
    check(gain::parseTags({{"replaygain_album_gain", "6 dB"}}, false).state == gain::TagState::Unsupported,
          "Album ReplayGain must not substitute for track values");
    check(gain::parseTags({{"replaygain_track_gain", "1 dB"}, {"replaygain_track_peak", "NaN"}}, false).state == gain::TagState::Invalid,
          "Nonfinite sample-peak tag must be rejected");
    check(gain::parseTags({{"replaygain_track_gain", "1 dB"}, {"REPLAYGAIN_TRACK_GAIN", "1 dB"}}, false).selected.has_value(),
          "Identical duplicate keys must not conflict");
    auto positiveTag = gain::parseTags({{"REPLAYGAIN_TRACK_GAIN", "+7.00 dB"}}, false);
    check(positiveTag.state == gain::TagState::Valid && positiveTag.selected &&
          positiveTag.selected->adjustmentDb == 7, "Signed positive ReplayGain was rejected");
    check(gain::choose(20, "unknown-peak", enabled, positiveTag, {}).appliedDb == 0,
          "Positive tag gain with unknown peak must be capped");
    const auto unverifiedPeak = gain::parseTags({{"REPLAYGAIN_TRACK_GAIN", "+7.00 dB"},
                                                {"REPLAYGAIN_TRACK_PEAK", "0.1"}}, false);
    check(gain::choose(20, "unverified-peak-domain", enabled, unverifiedPeak, {}).appliedDb == 0,
          "An unverified source-domain tag peak authorized a decoded-domain boost");
    check(gain::choose(21, "trusted-peak", enabled, positiveTag, {std::nullopt, 1.0}).appliedDb == -1,
          "Measured same-domain peak must constrain a tag-based decision");
    gain::Lock lock;
    lock.lockLoaded(measured);
    check(!lock.lockLoaded(gain::choose(10, "song-a", enabled, noTag, {-14, 1})) &&
          lock.current()->appliedDb == measured.appliedDb, "Mid-song result changed locked gain");
    // Waveform, catalog, pause, seek and queue edits never touch the lock.
    check(lock.current()->serial == 10 && lock.current()->appliedDb == 7, "Lock changed on unrelated events");
    check(lock.rollback(14,"song-a") && lock.rollback(14,"song-a")->appliedDb == 7, "Rollback gain");
    check(!lock.rollback(14,"other"), "Wrong-source rollback");
    lock.lockLoaded(gain::choose(15, "song-a", enabled, noTag, {-24, 0.2}));
    check(lock.current()->serial == 15, "New playback not unlocked");
    lock.overrideSettings(gain::choose(15, "song-a", disabled, noTag, {-24, 0.2}));
    check(lock.current()->appliedDb == 0, "User explicit disable");
    std::cout << "PASS gain: serial lock, fallback, tag precedence, R128, RG, cap, explicit toggle, unsupported Sound Check\n";
}
static void dspTests() {
    std::vector<float> pcm(48000*2*3);
    const double amp = std::pow(10., -23./20);
    for (size_t i = 0; i < pcm.size()/2; ++i) {
        const auto value = static_cast<float>(amp * std::sin(2*3.141592653589793*1000*i/48000));
        pcm[2*i] = pcm[2*i+1] = value;
    }
    auto original = analysis::measureSamples(pcm);
    check(original.loudness && std::abs(*original.loudness + 23) < 0.12, "DSP reference tone");
    for (auto& v : pcm) v *= 0.5f;
    auto half = analysis::measureSamples(pcm);
    check(half.loudness && std::abs(*original.loudness - *half.loudness - 6.0206) < .001, "DSP loudness scale");
    auto silence = analysis::measureSamples(std::vector<float>(48000*2,0));
    check(!silence.loudness, "DSP silence");
    rejects([&] { analysis::measureSamples({0}); }, "DSP truncation");
    rejects([&] { analysis::measureSamples({0,std::numeric_limits<float>::infinity()}); }, "DSP nonfinite");
    std::cout << "PASS DSP: reference -23 LUFS tone, -6.02 dB scale, silence and invalid frames\n";
}
int main() {
    std::cout << "Running portable gain and DSP checks\n" << std::flush;
    try { gainTests(); dspTests(); }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    std::cout << "ALL PORTABLE ANALYSIS TESTS PASS\n";
}
