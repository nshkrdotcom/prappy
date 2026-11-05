#include "analysis.h"
#include "audio.h"
#include "gain_policy.h"


#include <chrono>
#include <iostream>
#include <thread>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "Usage: prappy_analysis_windows_tests <generated-fixture-directory>");
        const fs::path root(argv[1]);
        std::cout << "fixtureRoot=" << utf8(root) << '\n';
        Audio audio;
        audio.open(true);
        std::cout << "mpvVersion=" << audio.textProperty("mpv-version") << '\n';
        require(audio.free_node_contents != nullptr, "Pinned mpv_free_node_contents missing");
        require(audio.textProperty("replaygain") == "no", "Playback ReplayGain auto mode enabled");
        struct Case { fs::path file; gain::TagState state; gain::Origin origin; };
        const Case cases[] = {
            {L"rg.flac", gain::TagState::Valid, gain::Origin::ReplayGain},
            {L"rg.mp3", gain::TagState::Valid, gain::Origin::ReplayGain},
            {L"rg.ogg", gain::TagState::Valid, gain::Origin::ReplayGain},
            {L"rg.m4a", gain::TagState::Valid, gain::Origin::ReplayGain},
            {L"r128.opus", gain::TagState::Valid, gain::Origin::R128},
            {L"header.opus", gain::TagState::Valid, gain::Origin::R128},
            {L"missing.flac", gain::TagState::Absent, gain::Origin::Fallback},
            {L"invalid.flac", gain::TagState::Invalid, gain::Origin::Fallback},
            {L"duplicate.flac", gain::TagState::Invalid, gain::Origin::Fallback},
            {L"soundcheck.m4a", gain::TagState::Unsupported, gain::Origin::Fallback},
            {fs::path(std::wstring(180, L'x')) / std::wstring(80, L'y') / L"long.flac", gain::TagState::Absent, gain::Origin::Fallback}
        };
        double opusLufs = 0, opusPeak = 0;
        for (const auto& test : cases) {
            const auto path = root / test.file;
            if (path.filename() == L"long.flac") require(path.wstring().size() > 260, "Long-path fixture is too short");
            require(audio.play(path, true), "Fixture load rejected");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!audio.loaded && audio.error.empty() && std::chrono::steady_clock::now() < deadline) {
                audio.poll(); std::this_thread::yield();
            }
            if (!audio.loaded || !audio.paused || audio.info.codec.empty())
                throw std::runtime_error("Codec unavailable before unpause: " + utf8(path) + " " + audio.error);
            require(audio.current == path, "Decoder path normalization changed the caller's source identity");
            const bool opus = audio.info.codec == "opus";
            const auto tags = gain::parseTags(audio.loadedTags(), opus);
            std::cout << utf8(path.filename()) << " codec=" << audio.info.codec << " keys=";
            for (const auto& [key, value] : audio.loadedTags()) std::cout << key << '=' << value << ';';
            std::cout << '\n' << std::flush;
            require(tags.state == test.state, "Unexpected pinned-decoder tag state");
            if (tags.selected) require(tags.selected->origin == test.origin, "Wrong tag policy origin");
            const auto cold = gain::choose(audio.loadedSerial, utf8(path), {true, -18, 0}, tags, {});
            require(cold.appliedDb == 0, "Unverified tagged peak permitted positive cold gain");
            gain::Tags measuredTags;
            std::string measuredCodec;
            const auto begin = std::chrono::steady_clock::now();
            const auto result = analysis::measure(path, {}, {}, [&](const auto& values, const std::string& codec) {
                measuredTags = gain::parseTags(values, codec == "opus"); measuredCodec = codec;
            });
            require(measuredCodec == audio.info.codec && measuredTags.state == tags.state,
                "Measurement metadata events differ from paused playback metadata");
            require(result.loudness && std::abs(result.duration() - 3) < .08, "Incomplete codec decode");
            require(std::abs(*result.loudness - (path.filename() == L"header.opus" ? -14 : -20)) < .6,
                "Measurement applied optional ReplayGain/R128 or lost mandatory header gain");
            const auto& peak = result.levels.back().front();
            const double samplePeak = std::max({std::abs(peak.leftMin), std::abs(peak.leftMax),
                                                std::abs(peak.rightMin), std::abs(peak.rightMax)});
            const auto warm = gain::choose(audio.loadedSerial, utf8(path), {true, -18, 0}, tags,
                                           {result.loudness, samplePeak});
            if (tags.selected) require(std::abs(warm.appliedDb - (opus ? 3 : 6)) < .001, "Wrong warm tag gain");
            audio.setGain(warm.appliedDb);
            audio.waitIdle(); // Inspect the acknowledged actor command, still before unpause.
            double actual = 0;
            require(audio.get_property(audio.handle, "volume-gain", MPV_FORMAT_DOUBLE, &actual) >= 0 &&
                    std::abs(actual - warm.appliedDb) < .001, "Pinned mpv did not apply volume-gain while paused");
            if (path.filename() == L"r128.opus") { opusLufs = *result.loudness; opusPeak = samplePeak; }
            if (path.filename() == L"header.opus") {
                require(std::abs(*result.loudness - opusLufs - 6) < .02 &&
                        std::abs(20 * std::log10(samplePeak / opusPeak) - 6) < .02,
                    "Opus mandatory header gain not applied exactly once");
            }
            std::cout << "PASS duration=" << result.duration() << " lufs=" << *result.loudness
                      << " peak=" << samplePeak << " coldDb=" << cold.appliedDb << " warmDb=" << actual
                      << " decodeMs=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count()
                      << '\n' << std::flush;
        }
        std::cout << "ALL PASS\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
