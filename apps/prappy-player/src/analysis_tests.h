#pragma once
#include "analysis.h"
#include "analysis_service.h"
#include "analysis_identity.h"
#include "library.h"
#include <cmath>
#include <limits>

inline void analysisTests(const fs::path& root, const Library& fixture, std::ostream& log) {
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto rejects = [&](const auto& action, const char* message) {
        bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
        require(failed, message);
    };
    {
        Library saved;
        Track source;source.path=root/"offline-preview.flac";source.id=key(source.path);source.bytes=100;source.stamp=20;source.offline=true;
        Track alias=source;alias.path=root/"offline-alias.flac";alias.id=key(alias.path);alias.bytes=200;alias.stamp=21;
        source.copies.push_back(alias.path);saved.tracks.push_back(source);saved.files={source,alias};
        require(!analysis::catalogIdentity(saved,source.path)&&!analysis::catalogIdentity(saved,alias.path),"Offline saved identity authorized playback gain.");
        const auto preview=analysis::catalogIdentity(saved,source.path,true),aliasPreview=analysis::catalogIdentity(saved,alias.path,true);
        require(preview&&preview->bytes==100&&aliasPreview&&aliasPreview->bytes==200&&aliasPreview->stamp==21,
            "Saved offline flags blocked local waveform previews or replaced alias-specific identity.");
    }
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

    std::string decodedCodec;
    const auto decoded = analysis::measure(identity.path, {}, {}, [&](const auto&, const std::string& codec) {
        decodedCodec = codec;
    });
    require(decodedCodec == "pcm_s16le", "Analysis metadata events lost source codec.");
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
    const auto directory = root / "analysis-v2";
    std::set<std::wstring> sources;
    for (const auto& file : fixture.files) sources.insert(key(file.path));
    for (const auto& track : fixture.tracks) sources.insert(key(track.path));
    auto collect = [&](analysis::Service& service, size_t required, bool initial) {
        std::vector<analysis::Summary> results;
        for (int i = 0; i < 6000 && results.size() < required; ++i) {
            if (initial) {
                for (auto& entry : service.initialSummaries()) results.push_back(std::move(entry));
            }
            for (auto& entry : service.collect()) results.push_back(std::move(entry));
            if (results.size() < required) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(results.size() >= required, "Analysis service did not publish expected completions.");
        return results;
    };
    {
        analysis::Service service(directory, {2, 1, 128});
        service.reconcile(fixture); service.prioritize(fixture.tracks.back());
        for (const auto& result : collect(service, sources.size(), false))
            require(result.error.empty() && !result.cached && result.duration > 0,
                "Fresh analysis service failed.");
    }
    {
        analysis::Service service(directory, {2, 1, 128}); service.reconcile(fixture);
        for (const auto& result : collect(service, sources.size(), true))
            require(result.error.empty() && result.cached, "Registry did not reuse cached summary.");
        const auto trace = service.diagnostics();
        require(std::none_of(trace.begin(), trace.end(), [](const auto& event) {
            return event.starts_with("start ");
        }), "Warm registry started a whole-song decoder.");
    }
    {
        auto offline = fixture;
        for (auto& track : offline.tracks) track.offline = true;
        for (auto& file : offline.files) file.offline = true;
        const auto saved = analysis::catalogIdentity(offline, identity.path, true);
        require(saved.has_value(), "Saved selected preview lost its source identity.");
        analysis::Service service(directory, {2, 1, 128}); service.reconcile(offline);
        service.prioritize(*saved);
        const auto previews = collect(service, 1, false);
        require(previews.front().cached && previews.front().waveform && previews.front().identity == *saved,
            "Offline saved selection did not restore its local waveform independently of scanning.");
        const auto trace = service.diagnostics();
        require(std::none_of(trace.begin(), trace.end(), [](const auto& event) { return event.starts_with("start "); }),
            "Offline preview unexpectedly opened a source decoder.");
        require(!analysis::catalogIdentity(offline, identity.path), "Offline preview authorized stale gain metadata.");
        log << "PASS saved offline selection: local waveform before scan, no source decode or gain authorization\n";
    }
    {
        analysis::Service service(directory, {2, 1, 128}); service.reconcile(fixture, true);
        for (const auto& result : collect(service, sources.size(), false))
            require(result.error.empty() && !result.cached, "Forced refresh reused old analysis.");
    }
    {
        analysis_registry::Entry entry;
        entry.path = analysis_registry::pathUtf8(identity.path);
        entry.bytes = identity.bytes; entry.stamp = identity.stamp;
        stateio::write(directory / analysis_registry::blobName(entry), "corrupt", false);
        analysis::Service service(directory, {2, 1, 128}); service.reconcile(fixture);
        // A cached summary is still immediately available even when a
        // selected waveform is damaged; only that blob is rebuilt.
        collect(service, sources.size(), true);
        service.prioritize(fixture.tracks.front());
        auto repairs = collect(service, 1, false);
        require(!repairs.front().cached && repairs.front().error.empty(),
            "Missing/corrupt waveform failed to trigger selected-track repair.");
    }
    {
        analysis::Service service(root / "cancelled-analysis-v2", {1, 1, 128});
        service.reconcile(fixture);
        // Destructor cancels decoding and joins without detached jobs.
    }
    const auto index = directory / "summaries.bin";
    const auto goodIndex = analysis_registry::readFile(index);
    const auto verifyRecovery = [&] {
        analysis::Service service(directory, {2, 1, 128}); service.reconcile(fixture);
        for (const auto& result : collect(service, sources.size(), true))
            require(result.cached && result.error.empty(), "Registry backup recovery decoded a source.");
    };
    stateio::write(fs::path(index.wstring() + L".bak"), goodIndex, false);
    auto corruptIndex = goodIndex; corruptIndex.back() ^= 1;
    stateio::write(index, corruptIndex, false);
    verifyRecovery();
    require(analysis_registry::decode(analysis_registry::readFile(index)).size() == sources.size(),
        "Recovered registry was not checkpointed on shutdown.");
    stateio::write(fs::path(index.wstring() + L".bak"), goodIndex, false);
    fs::remove(index); // Disposable test registry only: interrupted replacement.
    verifyRecovery();
    log << "PASS analysis streaming/registry/service: decode/cancel, warm summary reuse, "
           "forced refresh, corrupt selected blob repair, backup/interrupted replacement recovery, orderly shutdown\n";
}
