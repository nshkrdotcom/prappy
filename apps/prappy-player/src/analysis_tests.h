#pragma once
#include "analysis.h"
#include "library.h"
#include <cmath>
#include <limits>

inline void analysisTests(const fs::path& root, const Library& fixture, std::ostream& log) {
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto rejects = [&](const auto& action, const char* message) {
        bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
        require(failed, message);
    };
    // 1 kHz stereo sine, -23 dBFS peak/channel: approximately -23 LUFS.
    std::vector<float> signal(48000 * 2 * 3);
    const auto amplitude = std::pow(10.0, -23.0 / 20);
    for (size_t frame = 0; frame < signal.size() / 2; ++frame) {
        const float value = static_cast<float>(amplitude * std::sin(static_cast<double>(frame) * 2 * 3.141592653589793 * 1000 / 48000));
        signal[frame * 2] = signal[frame * 2 + 1] = value;
    }
    const auto tone = analysis::measureSamples(signal);
    require(tone.frames == 144000 && tone.loudness && std::abs(*tone.loudness + 23) < .12, "K-weighted sine reference mismatch.");
    require(tone.levels[0].size() == 141 && tone.levels.back().size() == 1 &&
        std::abs(tone.levels.back()[0].leftMax - amplitude) < .00001 &&
        std::abs(tone.levels.back()[0].rightMin + amplitude) < .00001, "Peak pyramid extrema/tail mismatch.");
    for (auto& value : signal) value *= .5f;
    const auto quieter = analysis::measureSamples(signal);
    require(quieter.loudness && std::abs(*tone.loudness - *quieter.loudness - 6.0206) < .001, "Loudness gain reference mismatch.");
    signal.insert(signal.end(), 48000 * 2 * 3, 0);
    const auto gated = analysis::measureSamples(signal);
    require(gated.loudness && std::abs(*gated.loudness - *quieter.loudness) < .5, "Relative silence gate failed.");
    const auto silence = analysis::measureSamples(std::vector<float>(48000 * 2, 0));
    require(!silence.loudness && silence.levels.back()[0].leftMax == 0, "Silence acquired finite loudness.");
    require(!analysis::measureSamples(std::vector<float>(100, .1f)).loudness, "Short audio acquired a complete loudness block.");
    rejects([&] { analysis::measureSamples({0, std::numeric_limits<float>::quiet_NaN()}); }, "Nonfinite samples accepted.");
    rejects([&] { analysis::measureSamples({0}); }, "Truncated stereo frame accepted.");

    const auto identity = analysis::identify(fixture.tracks[0].path);
    const auto bytes = analysis::serialize(identity, tone);
    require(analysis::parse(bytes, identity).frames == tone.frames, "Analysis cache round trip failed.");
    auto stale = identity; ++stale.bytes;
    rejects([&] { analysis::parse(bytes, stale); }, "Stale length accepted.");
    stale = identity; ++stale.stamp;
    rejects([&] { analysis::parse(bytes, stale); }, "Stale timestamp accepted.");
    stale = identity; stale.path = fixture.tracks[1].path;
    rejects([&] { analysis::parse(bytes, stale); }, "Wrong source/cache collision accepted.");
    auto damaged = bytes; damaged.back() ^= 1;
    rejects([&] { analysis::parse(damaged, identity); }, "Corrupt analysis accepted.");
    damaged = bytes; damaged[8] = 2;
    rejects([&] { analysis::parse(damaged, identity); }, "Unknown analysis schema accepted.");
    rejects([&] { analysis::parse(bytes.substr(0, 40), identity); }, "Truncated analysis accepted.");
    auto invalid = tone; invalid.levels.pop_back();
    rejects([&] { analysis::parse(analysis::serialize(identity, invalid), identity); }, "Incomplete pyramid accepted.");
    invalid = tone; invalid.levels.back()[0].leftMax = 5;
    rejects([&] { analysis::parse(analysis::serialize(identity, invalid), identity); }, "Inconsistent pyramid accepted.");
    log << "PASS analysis DSP/cache: reference tone, gain, gates, silence, short/nonfinite input, extrema, stale/corrupt/versioned cache\n" << std::flush;

    const auto decoded = analysis::measure(identity.path);
    require(std::abs(decoded.duration() - 3) < .02 && decoded.loudness && decoded.levels.back()[0].leftMax > .02f,
        "Streaming decoder duration/loudness/peaks failed.");
    std::stop_source stopped; stopped.request_stop();
    rejects([&] { analysis::measure(identity.path, stopped.get_token()); }, "Cancelled decode started.");
    std::stop_source inFlight;
    rejects([&] { analysis::measure(identity.path, inFlight.get_token(), [&](uint64_t frames) {
        if (frames >= 4096) inFlight.request_stop();
    }); }, "In-flight decode ignored cancellation.");
    require(inFlight.stop_requested(), "In-flight cancellation never reached decoded frames.");
    rejects([&] { analysis::measure(root / "missing.wav"); }, "Missing source analysis succeeded.");
    const auto broken = root / "broken-audio.wav"; { std::ofstream file(broken); file << "broken audio"; }
    rejects([&] { analysis::measure(broken); }, "Invalid audio analysis succeeded.");
    const auto directory = root / "analysis";
    auto collect = [&](analysis::Worker& worker) {
        std::vector<analysis::Summary> results;
        for (int i = 0; i < 3000 && results.size() < fixture.tracks.size(); ++i) {
            for (auto& result : worker.collect()) results.push_back(std::move(result));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(results.size() == fixture.tracks.size(), "Analysis worker stalled.");
        return results;
    };
    {
        analysis::Worker worker(directory); worker.reconcile(fixture); worker.prioritize(fixture.tracks.back());
        for (const auto& result : collect(worker)) require(result.error.empty() && !result.cached && result.duration > 0, "Fresh analysis worker failed.");
    }
    {
        analysis::Worker worker(directory); worker.reconcile(fixture);
        for (const auto& result : collect(worker)) require(result.error.empty() && result.cached, "Analysis worker did not reuse cache.");
    }
    {
        analysis::Worker worker(directory); worker.reconcile(fixture, true);
        for (const auto& result : collect(worker)) require(result.error.empty() && !result.cached, "Forced refresh reused analysis.");
    }
    stateio::write(analysis::cachePath(directory, identity), "corrupt", false);
    {
        analysis::Worker worker(directory); worker.reconcile(fixture);
        for (const auto& result : collect(worker)) require(result.error.empty() && (result.identity != identity || !result.cached), "Corrupt cache was not rebuilt.");
    }
    {
        analysis::Worker worker(root / "cancelled-analysis"); worker.reconcile(fixture);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    log << "PASS analysis streaming/worker: decode, invalid/missing source, cancellation, fresh/cache reuse, corrupt-cache rebuild, shutdown\n";
}
