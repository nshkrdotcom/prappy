#include "tests.h"
#include "analysis.h"
#include "playback.h"
#include "scan.h"
#include "status.h"
#include "feature_tests.h"
#include "text.h"
#include "ui_schedule.h"
#include "waveform_tests.h"
#include "sample_library_tests.h"
#include "sample_handoff_tests.h"
#include "sample_workspace_tests.h"
#include "audio_async_tests.h"
#include "sample_client_cancel_tests.h"
#include "scan_async_tests.h"
#include "debug_log_tests.h"
#include "evaluation_state.h"
#include "native_handoff_tests.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <fstream>
// An explicitly invoked real-device test; the fixture alternates one second of
// silence/tone for six seconds, then stays on. Never used by silent state tests.
int outputMeterTests(const fs::path& signal, const fs::path& logPath) {
    std::ofstream log(logPath);
    const auto require = [](bool value,const char* message) { if (!value) throw std::runtime_error(message); };
    try {
        Audio audio; audio.volume = 70;
        audio.open(false,{{"audio-buffer","1"},{"ao","wasapi"},{"audio-exclusive","no"}});
        require(audio.play(signal),"Cannot play meter timing fixture.");
        int tone = 0, silence = 0, wrong = 0;
        const auto start = Audio::Clock::now();
        while (std::chrono::duration<double>(Audio::Clock::now()-start).count() < 9) {
            audio.poll();
            double position = 0;
            if (audio.loaded && audio.get_property(audio.handle,"audio-pts",MPV_FORMAT_DOUBLE,&position) >= 0 &&
                position >= 1 && position < 6) {
                const double fraction = position - std::floor(position);
                if (fraction > .2 && fraction < .8) {
                    const bool audible = static_cast<int>(position)%2 != 0;
                    if (audible) ++tone; else ++silence;
                    if ((audio.meterPeak > .001) != audible) ++wrong;
                }
            }
            if (position >= 6.3) break;
            SDL_Delay(10);
        }
        log << "timing samples tone=" << tone << " silence=" << silence << " wrong=" << wrong << '\n' << std::flush;
        require(tone > 100 && silence > 60 && wrong < (tone+silence)/20,"Output meter does not follow audible timestamps with one-second decode buffering.");
        const auto settle = [&] {
            double maximum = 0;
            for (int i = 0; i < 50; ++i) { audio.poll(); if (i > 25) maximum = std::max(maximum,audio.meterPeak); SDL_Delay(10); }
            return maximum;
        };
        const auto normal = settle(); require(normal > .001,"Real device produced no session peak.");
        audio.setGain(-6); const auto reduced = settle();
        log << "gain ratio=" << reduced/normal << '\n';
        require(reduced/normal > .4 && reduced/normal < .6,"Output meter ignores application gain.");
        audio.setVolume(0); const auto muted=settle();
        log << "muted peak=" << muted << " mpv volume=" << audio.textProperty("volume")
            << " audio-pts=" << audio.textProperty("audio-pts") << '\n' << std::flush;
        require(muted < .00001,"Zero volume still shows output signal.");
        require(!audio.meterAvailable,"Zero-volume output meter remains active.");
        audio.setVolume(70); audio.setGain(0); require(settle() > .001,"Restored volume lost meter.");
        audio.setPaused(true); require(settle() == 0,"Paused output meter remains active.");
        audio.seek(.3); settle(); audio.setPaused(false);
        require(settle() < .00001,"Seek into silence retained pre-seek peaks.");
        audio.seek(7); require(settle() > .001,"Seek into tone lost output meter.");
        log << "PASS real WASAPI output timing, gain, zero volume, pause, resume and exact seek\nALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}
static void playbackStateTests(const fs::path& root, Library& fixture, std::ostream& log) {
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        Audio audio; audio.open(true);
        History visits; HistoryFile sessionFile(root / "session.txt"); Playback player(audio, fixture, visits, sessionFile,fixturePlayback(audio));
        double availableLufs = -24;
        player.decideGain = [&](const fs::path& path, uint64_t serial) {
            int paused = 0;
            require(audio.get_property(audio.handle, "pause", MPV_FORMAT_FLAG, &paused) >= 0 && paused,
                "Gain selection ran after playback unpaused.");
            return gain::choose(serial, utf8(path), {true, -18, 0}, {},
                {availableLufs, 0.1});
        };
        const auto actualGain = [&] {
            audio.waitIdle();
            double value = 0;
            require(audio.get_property(audio.handle, "volume-gain", MPV_FORMAT_DOUBLE, &value) >= 0,
                "Pinned libmpv volume-gain property unavailable.");
            return value;
        };
        const auto wait = [&](Playback& session, Audio& source) {
            for (int i = 0; i < 1000; ++i) {
                session.tick();
                if (!session.pending && source.loaded) return;
                SDL_Delay(5);
            }
            throw std::runtime_error("Playback state timeout: " + session.notice + " " + source.error);
        };
        const auto play = [&](const fs::path& path) { require(player.request(path), "Playback request failed."); wait(player, audio); };
        play(fixture.tracks[0].path); const auto visitA = visits.current()->id;
        const auto gainEpochA = audio.loadedSerial;
        require(player.gainDecision.current() && player.gainDecision.current()->serial == gainEpochA &&
            std::abs(actualGain() - 6) < .001, "Gain was not locked before playback unpaused.");
        availableLufs = -26; // A late result is for the NEXT playback only.
        for (int i = 0; i < 5; ++i) player.tick();
        audio.setPaused(true); audio.seek(.5); audio.setPaused(false);
        require(player.gainDecision.current()->serial == gainEpochA && std::abs(actualGain() - 6) < .001,
            "Late loudness, pause/resume or seek changed gain within a playback epoch.");
        audio.setVolume(23);
        require(std::abs(audio.gain - 6) < .001, "User volume changed application normalization.");
        audio.setVolume(70);
        for (int i = 0; i < 400 && (audio.info.codec.empty() || audio.info.sampleFormat.empty() || audio.info.sampleRate <= 0 || audio.info.channels <= 0); ++i) {
            SDL_Delay(5); player.tick();
        }
        require(audio.info.codec == "pcm_s16le" && audio.info.sampleRate == 44100 && audio.info.channels == 1 &&
            !audio.info.sampleFormat.empty() && audio.info.describe().find("44100 Hz") != std::string::npos,
            "Source audio format details are missing or incorrect.");
        play(fixture.tracks[1].path); const auto visitB = visits.current()->id;
        require(player.gainDecision.current() && player.gainDecision.current()->serial == audio.loadedSerial &&
            std::abs(actualGain() - 8) < .001, "Next playback did not use the available late measurement.");
        play(fixture.tracks[0].path); const auto visitC = visits.current()->id;
        require(player.navigate(-1), "Back unavailable."); wait(player, audio);
        require(visits.current()->id == visitB && visits.entries.size() == 3, "Back added a visit.");
        require(player.navigate(-1), "Second Back unavailable."); wait(player, audio);
        require(visits.current()->id == visitA && !player.adjacent(-1), "Back boundary incorrect.");
        require(player.navigate(1), "Forward unavailable."); wait(player, audio);
        require(visits.current()->id == visitB, "Forward chose library instead of trail.");
        play(fixture.tracks[1].path);
        require(visits.trail.size() == 3 && visits.find(visitC) && visits.trail[1] == visitB, "New branch lost old history.");
        const auto beforeRapid = visits.entries.size();
        require(player.request(fixture.tracks[0].path) && player.request(fixture.tracks[1].path) && player.request(fixture.tracks[0].path), "Rapid requests failed.");
        wait(player, audio);
        require(visits.entries.size() == beforeRapid + 1 && key(visits.current()->path) == key(fixture.tracks[0].path), "Stale load events committed wrong visits.");
        require(player.request(fixture.tracks[0].path), "Pause-during-load request failed.");
        player.toggle(); wait(player, audio);
        require(audio.paused, "Pause intent during loading was lost.");
        player.toggle(); require(!audio.paused, "Resume after pending pause failed.");
        const auto beforeMissing = visits.entries.size(), currentId = visits.current()->id;
        const auto previousGain = player.gainDecision.current()->appliedDb;
        availableLufs = -22; // Rollback must preserve the previous listening gain.
        require(player.request(root / "missing.wav"), "Missing request not accepted by mpv."); wait(player, audio);
        require(visits.entries.size() == beforeMissing && visits.current()->id == currentId && !player.notice.empty(), "Failed load changed history or lost error.");
        require(player.gainDecision.current() && std::abs(actualGain() - previousGain) < .001,
            "Failed-load rollback silently renormalized the previous listening session.");
        const auto invalidAudio = root / "invalid.wav"; { std::ofstream bad(invalidAudio); bad << "not an audio file"; }
        require(player.request(invalidAudio), "Invalid media request failed early."); wait(player, audio);
        require(visits.entries.size() == beforeMissing && visits.current()->id == currentId, "Decoder error recorded a visit.");
        audio.pause();
        const auto seekStart = Audio::Clock::now(); const auto requestsBefore = audio.seekSequence;
        for (int i = 0; i < 10000; ++i) audio.seek(static_cast<double>(i%19)/10);
        audio.seek(.8);
        require(audio.seekSequence == requestsBefore+1 && audio.queuedSeek == .8 && audio.displayPosition() == .8,
            "Live rapid seeking blocked coalescing or optimistic feedback.");
        log << "Seek submission: 10001 targets coalesced in " << std::chrono::duration<double,std::milli>(Audio::Clock::now()-seekStart).count() << " ms\n";
        for (int i = 0; i < 200 && (audio.pendingSeek||audio.queuedSeek||std::abs(audio.position - .8) > .25); ++i) { player.tick(); SDL_Delay(5); }
        require(audio.paused && audio.position > .5, "Cannot checkpoint paused position.");
        double decoded = 0;
        require(audio.get_property(audio.handle,"time-pos",MPV_FORMAT_DOUBLE,&decoded) >= 0 && std::abs(decoded-.8) < .25,
            "Seek preview hid an incorrect decoder position.");
        player.checkpoint(); visits.repeat = RepeatMode::One; ++visits.revision; player.finish();
        HistoryFile restoredFile(root / "session.txt"); auto restoredHistory = restoredFile.load(10000);
        Audio restoredAudio; restoredAudio.open(true); Playback restored(restoredAudio, fixture, restoredHistory, restoredFile,fixturePlayback(restoredAudio));
        restored.restore(); wait(restored, restoredAudio);
        require(restoredAudio.paused && restoredHistory.entries.size() == visits.entries.size() && restoredAudio.position > .5 && restoredHistory.repeat == RepeatMode::One, "Session did not restore paused without adding visits.");
        restoredAudio.waitIdle();int paused = 0; restoredAudio.get_property(restoredAudio.handle, "pause", MPV_FORMAT_FLAG, &paused);
        require(paused != 0, "Restored mpv was not paused.");
        log << "PASS playback: branch/Back/Forward, rapid requests, locked gain vs late results/seek/pause, "
               "next-play gain, independent volume, failed-load gain rollback, paused restore/repeat\n";

        const auto awaitEofAction = [&](Playback& session, Audio& source, uint64_t previous) {
            for (int i = 0; i < 1400; ++i) {
                session.tick(); if (source.serial > previous && !session.pending && source.loaded) return;
                SDL_Delay(5);
            }
            throw std::runtime_error("EOF repeat/progression timeout.");
        };
        const auto sameVisit = restoredHistory.current()->id;
        const auto sameCount = restoredHistory.entries.size();
        restored.toggle(); restoredAudio.seek(restoredAudio.duration - .12); const auto beforeLoop = restoredAudio.serial;
        awaitEofAction(restored, restoredAudio, beforeLoop);
        require(restoredHistory.current()->id == sameVisit && restoredHistory.entries.size() == sameCount, "Repeat One added history entries.");
        require(restored.navigate(1), "Repeat One intercepted manual Next."); wait(restored, restoredAudio);
        require(restoredHistory.entries.size() == sameCount + 1, "Manual Next failed while repeating One.");
        restoredHistory.repeat = RepeatMode::All; ++restoredHistory.revision;
        require(restored.request(fixture.tracks.back().path), "Cannot select final library track."); wait(restored, restoredAudio);
        restoredAudio.seek(restoredAudio.duration - .12); const auto beforeAll = restoredAudio.serial;
        awaitEofAction(restored, restoredAudio, beforeAll);
        require(key(restoredAudio.current) == fixture.tracks.front().id, "Repeat All did not wrap library.");
        restoredHistory.repeat = RepeatMode::Off; ++restoredHistory.revision;
        require(restored.request(fixture.tracks.back().path), "Cannot select final track for repeat Off."); wait(restored, restoredAudio);
        restoredAudio.seek(restoredAudio.duration - .12); const auto beforeOff = restoredAudio.serial;
        for (int i = 0; i < 1400 && restoredAudio.active; ++i) { restored.tick(); SDL_Delay(5); }
        require(!restoredAudio.active && !restored.pending && restoredAudio.serial == beforeOff, "Repeat Off did not stop at library end.");
        require(restored.navigate(-1), "Cannot traverse while testing concurrent pruning.");
        restoredHistory.setLimit(1); wait(restored, restoredAudio);
        require(restoredHistory.entries.size() == 1 && restoredHistory.current(), "Pruned pending target caused invalid navigation.");
        Library one; one.tracks.push_back(fixture.tracks.front());
        History oneHistory; oneHistory.repeat = RepeatMode::All;
        HistoryFile oneFile(root / "one.txt"); Audio oneAudio; oneAudio.open(true); Playback onePlayer(oneAudio, one, oneHistory, oneFile,fixturePlayback(oneAudio));
        require(onePlayer.navigate(1), "Single-track start failed."); wait(onePlayer, oneAudio);
        oneAudio.seek(oneAudio.duration - .12); const auto beforeOne = oneAudio.serial;
        awaitEofAction(onePlayer, oneAudio, beforeOne);
        require(oneHistory.entries.size() == 2 && key(oneAudio.current) == one.tracks.front().id, "Single-track Repeat All failed.");
        Library empty; History noHistory; HistoryFile noFile(root / "empty.txt"); Playback noPlayer(oneAudio, empty, noHistory, noFile,fixturePlayback(oneAudio));
        require(!noPlayer.navigate(1), "Empty library should not start/retry playback.");
        restored.finish(); onePlayer.finish(); player.finish();
        log << "PASS repeat: One preserves visits/manual navigation; All wraps; Off stops; one-track and empty libraries\n";
}

// Integration checks run only in explicit --state-test fixture directories.
int stateTests(const fs::path& configPath, const fs::path& logPath) {
    std::ofstream log(logPath);
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto rejects = [&](const auto& operation, const char* message) {
        bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
        require(rejected, message);
    };
    try {
        const auto defaults=logPath.parent_path()/fs::path("defaults-"+sampling::uuid()+".ini");
        createDefaultConfig(defaults);
        require(readConfig(defaults).debugLog,"Generated player defaults do not load with debug logging enabled.");
        {
            const auto scope=logPath.parent_path()/fs::path("evaluation-scope-"+sampling::uuid());
            const auto stable=scope/"stable",trial=scope/"trial";
            fs::create_directories(stable);fs::create_directories(trial);
            sampling::Json marker={{"mode","indexed-evaluation"},{"protocol",3},{"state_root",utf8(trial)},
                {"source_state_root",utf8(stable)},{"url","http://127.0.0.1:18816"},{"token_file",utf8(scope/"trial.token")}};
            stateio::write(trial/"evaluation-state.json",marker.dump());
            require(!stateio::evaluationState(trial,true).is_null(),"Evaluation state scope validation failed.");
            stateio::evaluationConnection(trial,"http://127.0.0.1:18816",scope/"trial.token");
            rejects([&]{stateio::evaluationConnection(trial,"http://127.0.0.1:8765",scope/"trial.token");},"Evaluation redirected to production.");
            rejects([&]{stateio::evaluationConnection(trial,"http://127.0.0.1:18816",scope/"production.token");},"Evaluation reused production credentials.");
            stateio::write(trial/"sampling-connection.json",sampling::Json{{"url",marker.at("url")},{"token_file",marker.at("token_file")},{"library",""}}.dump());
            auto selected=std::make_shared<prepared::CatalogSource>();selected->source={"lib_"+std::string(32,'a'),"cap_"+std::string(32,'b')};
            selected->asset=std::string(64,'c');selected->state="Playable";selected->native={44100,1,441000,prepared::SampleType::Float32};
            Audio playing;playing.current=trial/"virtual.source";playing.duration=10;playing.position=2;playing.preparedSource=selected;
            {
                sampling::Workspace readonly(trial,[]{},sampling::PcmBudget{64});readonly.mark(playing,sampling::SecondsSpan{1.25,2.5});
                const auto state=sampling::Json::parse(readonly.diagnostics());
                require(state.at("pcm_budget_bytes")==64*1024*1024,"Configured editor memory did not reach the workspace.");
                require(state.at("read_only")&&state.at("mark_pending"),"Evaluation did not retain its mark in memory.");
            }
            require(!fs::exists(trial/"sample-workspaces.json"),"Read-only evaluation persisted an editing gesture.");
            marker["source_state_root"]=utf8(trial);stateio::write(trial/"evaluation-state.json",marker.dump());
            rejects([&]{stateio::evaluationState(trial,true);},"Evaluation accepted overlapping stable state.");
            log<<"PASS evaluation state: distinct roots, fixed endpoint and independent credentials\n";
        }
        nativeHandoffTests(logPath.parent_path()/sampling::uuid(),log);
        waveformRenderTests(log);
        {
            require(Browse::tokens("  artist \"NU NU\" mix ") == std::vector<std::wstring>({L"artist",L"nu nu",L"mix"}),
                "Quoted phrase was split or kept its quotes.");
            Track phrase, separated;
            phrase.search = phrase.searchName = L"nu nu mix.wav";
            separated.search = separated.searchName = L"nu other nu mix.wav";
            const auto quoted = Browse::tokens("\"nu nu\"");
            require(Browse::score(phrase,quoted) >= 0 && Browse::score(separated,quoted) < 0 &&
                Browse::score(separated,Browse::tokens("nu nu")) >= 0,"Phrase search did not require contiguous spaces.");
            require(Browse::tokens("\"nu nu") == quoted && Browse::tokens("\"\"").empty(),"Partial/empty quote handling failed.");
            log << "PASS quoted phrase search: exact spaces, case folding, mixed terms and unfinished quotes\n";
        }
        {
            History h;const auto path=fs::absolute(L"history-test.wav");
            const auto old=h.add(path);h.add(path);h.visit(0);h.add(path);
            const auto retained=h.entries[1].id;const auto count=h.playedCount,next=h.nextId;
            h.revisit(retained);
            require(h.current()->id==retained&&h.playedCount==count&&h.nextId==next,"Retained branch replay duplicated a visit.");
            h.revisit(old);require(h.cursor==0&&h.playedCount==count,"Existing trail replay duplicated a visit.");
            auto reopened=History::parse(h.serialize(),10000);require(reopened.current()->id==old,"Revisited cursor did not persist.");
            log<<"PASS existing and retained-branch history replay preserves entry identities/counts and cursor persistence\n";
        }

        {
            text::Painter painter(nullptr);
            const auto trimmed = painter.inspect("Long folder 中文 👩🏽‍🔬 with combining café and more", {},18,85);
            require(trimmed.trimmed && trimmed.width <= 85 && !trimmed.missingGlyph, "Native ellipsis failed to bound a shaped Unicode label.");
            for (const auto* label : {"中文 日本語 한국어", "😀 👩🏽‍🔬 🇯🇵 👨‍👩‍👧‍👦", "café café العربية"}) {
                const auto shaped = painter.inspect(label, {}, 18);
                require(shaped.width > 0 && !shaped.missingGlyph, "Unicode shaping produced missing glyphs.");
            }
            const auto emoji = painter.inspect("👩🏽‍🔬", {L"🔬"}, 18);
            require(emoji.clusters.size() == 1 && emoji.highlightedClusters == 1 && emoji.clusters[0] == 7,
                "Emoji highlighting split a shaped cluster.");
            require(painter.inspect("文件", {}, 36).width > painter.inspect("文件", {}, 18).width * 1.9f,
                "Text size did not scale.");
            UiSchedule schedule; const auto now = UiSchedule::Clock::now();
            for (int frame = 0; frame < 3; ++frame) {
                painter.beginFrame();
                const auto before = painter.cacheStats();
                for (int i = 0; i < 160; ++i) {
                    const auto label = "Visible Unicode row " + std::to_string(i);
                    painter.width(label,18);
                    painter.inspect(label,{L"row"},18);
                    painter.inspect(label,{},18);
                }
                const auto after = painter.cacheStats();
                require(after.layouts >= 160 && after.shaped >= 160,"Text layout cache diagnostics missing.");
                if (frame) require(after.shaped == before.shaped,"Tall viewport reshapes visible text every frame.");
            }
            schedule.dirty = false; schedule.settle = 0; schedule.paint = now;
            require(schedule.wait(now, true, false, false, false, {}) == 1000 && !schedule.due(now, false), "Idle UI paints on watchdog.");
            schedule.caret = now + std::chrono::milliseconds(400);
            require(schedule.wait(now, true, false, false, false, {}) == 400 && schedule.due(schedule.caret, false), "Caret deadline lost.");
            require(schedule.wait(now, false, false, false, false, {}) == 1000, "Hidden UI paints caret.");
            schedule.presented(now); schedule.input(now+std::chrono::milliseconds(1));
            require(schedule.due(now+std::chrono::milliseconds(1),false), "Input waits behind animation frame cap.");
            require(std::abs(releasePeak(releasePeak(1, 0, .1), 0, .2) - releasePeak(1, 0, .3)) < .00001,
                "Meter depends on frame rate.");
            log << "PASS Unicode shaping, emoji clusters, scaled text, idle/caret deadlines and elapsed-time audio controls\n";
        }
        {
            static std::vector<double> submitted;
            submitted.clear();
            Audio simulated; simulated.loaded = simulated.active = true; simulated.duration = 100;
            simulated.command_async = [](mpv_handle*,uint64_t,const char** args) { submitted.push_back(std::stod(args[1])); return 0; };
            simulated.seek(10);
            for (int i = 0; i < 10000; ++i) simulated.seek(static_cast<double>(i%99));
            require(submitted.size() == 1 && simulated.queuedSeek == 0 && simulated.displayPosition() == 0,
                "Rapid seeks were not coalesced or the marker waited for decoding.");
            simulated.pendingSeek = 0; simulated.submitSeek();
            require(submitted.size() == 2 && submitted.back() == 0 && !simulated.queuedSeek, "Latest seek target was lost.");
            simulated.duration = 0; simulated.seek(12);
            require(simulated.displayPosition() == 12 && simulated.queuedSeek == 12, "Restore seek was lost before duration became available.");
            simulated.duration = 100;
            simulated.seekPreview.reset(); simulated.position = 20; simulated.positionTime = Audio::Clock::now();
            require(std::abs(simulated.displayPosition(simulated.positionTime+std::chrono::milliseconds(50))-20.05) < .00001,
                "Playback marker did not interpolate between decoder updates.");
            simulated.paused = true;
            require(simulated.displayPosition(simulated.positionTime+std::chrono::seconds(2)) == 20, "Paused marker continued advancing.");
            simulated.active = false; simulated.pauseRequested = true; simulated.setPaused(false);
            require(!simulated.pauseRequested,"Committing a tiny clip after EOF lost the explicit play intent.");
            log << "PASS asynchronous seek coalescing, immediate preview, smooth clock and paused marker\n";
        }
        require(statusText("", "Checking duplicates: 4 / 10") == "Checking duplicates: 4 / 10", "Empty status adds a leading separator.");
        require(statusText("10 tracks | 2 identical copies hidden", "Checking duplicates: 4 / 10") ==
            "10 tracks \xc2\xb7 2 identical copies hidden \xc2\xb7 Checking duplicates: 4 / 10", "Status sections are not joined with middle dots.");
        const auto root = logPath.parent_path() / ("state-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(root);
        debugLogTests(root,log);
        audio_async_tests::run(log);
        sampleClientCancellationTests(log);
        scanAsyncTests(log);
        sampleLibraryTests(root,log);
        sampleWorkspaceTests(root,log);
        pendingSampleHandoffTests(root,log);
        {
            // Public-domain test signal: 3072 stereo silent frames, 48 kHz/24-bit
            // FLAC generated with SoX, retaining STREAMINFO and its one audio frame.
            const unsigned char bytes[] = {0x66,0x4c,0x61,0x43,0x80,0,0,0x22,0x10,0,0x10,0,0,0,0x12,0,0,0x12,0x0b,0xb8,0x03,0x70,0,0,0x0c,0,0xf9,0xde,0xbe,0x3f,0x07,0xbe,0x68,0x53,0x3b,0xf0,0x29,0x5e,0x3d,0x2b,0xa6,0x8a,0xff,0xf8,0x7a,0x1c,0,0x0b,0xff,0x9c,0,0,0,0,0,0,0,0,0x3f,0xff};
            const auto clip = root / "short.flac";
            { std::ofstream output(clip,std::ios::binary); output.write(reinterpret_cast<const char*>(bytes),sizeof(bytes)); }
            Library clips; Track track; track.path = clip; track.id = key(clip); clips.tracks.push_back(track);
            const auto nextClip = root / "next-short.flac"; fs::copy_file(clip,nextClip);
            track.path = nextClip; track.id = key(nextClip); clips.tracks.push_back(track);
            History visits; visits.add(clip); HistoryFile storage(root / "short-history.txt");
            Audio source; source.open(true); Playback session(source,clips,visits,storage,fixturePlayback(source)); session.restore();
            int64_t outputRate = 0;
            for (int i = 0; i < 400; ++i) {
                session.tick();
                if (source.loaded && source.get_property(source.handle,"audio-out-params/samplerate",MPV_FORMAT_INT64,&outputRate) >= 0 && outputRate > 0) break;
                // The null output can consume an entire 64ms clip during priming,
                // even paused. Its clean EOF is valid; actual samples are checked below.
                if (source.sequentialRetry && !source.active && source.error.empty()) break;
                SDL_Delay(5);
            }
            require(source.paused && !session.pending && (source.loaded || (source.sequentialRetry && !source.active)) &&
                source.error.empty() && session.notice.empty() && visits.playedCount == 1,"Short FLAC startup restore produced a playback error.");
            session.toggle();
            for (int i = 0; i < 400 && source.active; ++i) { session.tick(); SDL_Delay(5); }
            require(!source.active && source.error.empty() && session.notice.empty(),"Short restored FLAC did not play to clean EOF.");
            const auto measured = analysis::measure(clip);
            require(measured.frames == 3072,"Short FLAC analysis lost decoded frames.");
            log << "PASS 64ms single-frame FLAC: paused startup restore, playback EOF and full-frame analysis\n";
        }
        const auto a = fs::absolute(root / L"space Beyonc\u00e9" / L"a.wav"), b = fs::absolute(root / "b.wav"), c = fs::absolute(root / "c.wav");
        History history;
        require(history.limit == 10000, "History default must be 10000.");
        const auto idA = history.add(a), idB = history.add(b), idC = history.add(c);
        history.visit(1); history.add(b);
        require(history.trail.size() == 3 && history.trail[0] == idA && history.trail[1] == idB && history.find(idC), "Branching discarded history or kept wrong navigation.");
        history.position(1.25); history.cycleRepeat();
        auto parsed = History::parse(history.serialize(), 10000);
        require(parsed.entries.size() == 4 && parsed.current()->position == 1.25 && parsed.repeat == RepeatMode::One && parsed.entries[0].path == a, "History round trip failed.");
        std::string windowsText = "\xef\xbb\xbf";
        for (char ch : history.serialize()) { if (ch == '\n') windowsText += '\r'; windowsText += ch; }
        require(History::parse(windowsText, 10000).entries.size() == 4, "Windows CRLF/BOM history rejected.");
        parsed.visit(0); parsed.setLimit(2);
        require(parsed.entries.size() == 2 && parsed.current()->id == idA && parsed.trail.size() == 2, "Pruning lost current old visit.");
        parsed.cycleRepeat(); require(parsed.repeat == RepeatMode::All, "Repeat One -> All failed.");
        parsed.cycleRepeat(); require(parsed.repeat == RepeatMode::Off, "Repeat All -> Off failed.");
        History large;
        for (int i = 0; i < 10005; ++i) large.add(a);
        require(large.entries.size() == 10000 && large.trail.size() == 10000 && large.entries.front().id == 6, "Default retention limit failed.");
        require(History::parse(large.serialize(), 10000).entries.size() == 10000, "Large history restore failed.");
        const auto snapshot = large.snapshot();
        large.position(2); large.add(b); large.visit(0); large.setLimit(1);
        const auto unchangedSnapshot = History::parse(snapshot.serialize(), 10000);
        require(unchangedSnapshot.entries.size() == 10000 && unchangedSnapshot.current()->position == 0 &&
            unchangedSnapshot.entries.front().id == 6, "Background snapshot changed during append/position/pruning.");
        rejects([&] { History::parse("PRAPPY_HISTORY 1\nENTRY 1 1 nan \"C:/bad.wav\"\n", 10000); }, "Malformed history accepted.");
        rejects([&] { History::parse(history.serialize().substr(0, 90), 10000); }, "Truncated history accepted.");
        rejects([&] { stateio::pathFromText(std::string("C:/bad\0path", 11)); }, "Embedded NUL path accepted.");
        rejects([&] { stateio::pathFromText("relative.wav"); }, "Relative saved path accepted.");
        auto unc = History{}; unc.add(fs::path(L"\\\\wsl.localhost\\ubuntu26-dev\\music\\space \u00e9.wav"));
        require(History::parse(unc.serialize(), 10000).current()->path == unc.current()->path, "UNC path round trip failed.");
        log << "PASS history: 10000 retention, branching, repeat cycle, Unicode paths, pruning, corruption bounds\n";

        const auto savedPath = root / "history.txt";
        stateio::write(savedPath, history.serialize());
        history.add(c); stateio::write(savedPath, history.serialize());
        require(History::parse(stateio::read(savedPath.wstring() + L".bak"), 10000).entries.size() == 4, "Backup rotation failed.");
        { std::ofstream broken(savedPath, std::ios::binary); broken << "truncated"; }
        HistoryFile recoveredFile(savedPath); auto recovered = recoveredFile.load(10000);
        require(recovered.entries.size() == 4 && recoveredFile.notice.find("Recovered") != std::string::npos, "History backup recovery failed.");
        recovered.add(c); recoveredFile.save(recovered, true, true);
        require(History::parse(stateio::read(savedPath.wstring() + L".bak"), 10000).entries.size() == 4, "Recovery overwrote good backup.");
        const auto futurePath = root / "future.txt"; stateio::write(futurePath, "PRAPPY_HISTORY 2\n");
        HistoryFile futureFile(futurePath); auto futureHistory = futureFile.load(10000); futureHistory.add(a); futureFile.save(futureHistory, true, true);
        require(!futureFile.writable && stateio::read(futurePath) == "PRAPPY_HISTORY 2\n", "Newer history version was overwritten.");
        const auto salvagePath = root / "salvage.txt";
        stateio::write(salvagePath, history.serialize() + "ENTRY broken\n");
        HistoryFile salvagedFile(salvagePath); auto salvaged = salvagedFile.load(10000);
        require(salvaged.entries.size() == history.entries.size() && salvagedFile.writable && salvagedFile.notice.find("damaged original") != std::string::npos, "Independent valid history entries were not recovered.");
        salvagedFile.save(salvaged, true, true);
        require(History::parse(stateio::read(salvagePath), 10000).entries.size() == history.entries.size(), "Salvaged history was not persisted.");
        HistoryFile unwritable(root / "no-parent" / "history.txt");
        unwritable.save(history, true, true); require(!unwritable.notice.empty(), "Save failure not surfaced.");
        const auto lockedPath = root / "locked.txt"; stateio::write(lockedPath, "original");
        const auto lock = CreateFileW(lockedPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "Cannot lock state fixture.");
        bool refused = false; try { stateio::write(lockedPath, "replacement"); } catch (const std::exception&) { refused = true; }
        CloseHandle(lock); require(refused && stateio::read(lockedPath) == "original", "Failed write destroyed old state.");
        log << "PASS storage: checked replacement, backups, recovery, future-version preservation, failed writes\n";

        Library fixture;
        for(const auto name:{"first.wav","second.wav"}){
            Track track;track.path=fs::absolute(configPath.parent_path()/name);track.id=key(track.path);
            require(fs::is_regular_file(track.path),"State test audio fixture is missing.");
            track.name=track.label=name;track.type="wav";track.sourceRoot=track.path.parent_path();
            fixture.tracks.push_back(std::move(track));
        }
        fixture.rebuildSearch();
        const auto media = root / "media"; fs::create_directories(media);
        const auto first = media / "a.wav", copy = media / "b.wav", other = media / "c.wav";
        fs::copy_file(fixture.tracks[0].path, first, fs::copy_options::overwrite_existing);
        fs::copy_file(fixture.tracks[0].path, copy, fs::copy_options::overwrite_existing);
        fs::copy_file(fixture.tracks[1].path, other, fs::copy_options::overwrite_existing);
        {
            Audio source;source.open(true);Library empty;History visits;HistoryFile storage(root/"history-selection.txt");Playback session(source,empty,visits,storage,fixturePlayback(source));
            const auto loaded=[&]{for(int i=0;i<1000;++i){session.tick();if(!session.pending&&source.loaded)return;SDL_Delay(5);}throw std::runtime_error("History replay load timed out.");};
            require(session.request(first),"History fixture failed.");loaded();const auto id=visits.current()->id;
            require(session.request(other),"Second history fixture failed.");loaded();
            require(session.selectHistory(id),"Default replay failed.");loaded();
            require(visits.playedCount==2&&visits.current()->id==id,"Default history selection added a new visit.");
            require(session.selectHistory(id,true),"Opt-in replay failed.");loaded();
            require(visits.playedCount==3&&visits.current()->id!=id,"Opt-in history selection did not add a visit.");
            log<<"PASS playback history selection: default reuse and opt-in new visit\n";
        }
        const auto localConfig = root / "player.ini";
        { std::ofstream config(localConfig); config << "history_limit=23\n"; }
        auto settings = readConfig(localConfig); require(settings.historyLimit == 23 && !settings.addNewListeningHistoryEntryWhenReplayingHistoryItem, "Configured history defaults failed.");
        {std::ofstream out(localConfig,std::ios::app);out<<"add_new_listening_history_entry_when_replaying_history_item=true\n";}
        settings=readConfig(localConfig);require(settings.addNewListeningHistoryEntryWhenReplayingHistoryItem,"History replay option failed.");
        const auto badConfig = root / "invalid.ini";
        { std::ofstream config(badConfig); config << "history_limit=-1\n"; }
        rejects([&] { readConfig(badConfig); }, "Invalid history limit accepted.");
        const auto tuning = root / "analysis-tuning.ini";
        const auto writeTuning = [&](const std::string& setting) {
            std::ofstream out(tuning);
            out << setting << '\n';
        };
        writeTuning("");require(readConfig(tuning).debugLog,"Debug logging must default on for existing configs.");
        require(readConfig(tuning).editorPcmMib==256,"Editor memory default changed.");
        for(const auto mib:{64,1024}){writeTuning("editor_pcm_mib="+std::to_string(mib));require(readConfig(tuning).editorPcmMib==static_cast<size_t>(mib),"Editor memory setting did not round trip.");}
        for(const auto invalid:{"63","1025","NaN","256extra","-1"}){writeTuning(std::string("editor_pcm_mib=")+invalid);rejects([&]{readConfig(tuning);},"Invalid editor memory limit was accepted.");}
        writeTuning("debug_log=false");require(!readConfig(tuning).debugLog,"Debug logging could not be disabled.");
        writeTuning("debug_log=true");require(readConfig(tuning).debugLog,"Debug logging could not be enabled.");
        writeTuning("debug_log=invalid");rejects([&]{readConfig(tuning);},"Invalid debug flag was accepted.");
        writeTuning("normalization_target_lufs=-20\nnormalization_unmeasured_fallback_db=-4\n"
            "debug_log=true");
        auto tuned = readConfig(tuning);
        require(tuned.normalizationTargetLufs == -20 && tuned.normalizationUnmeasuredFallbackDb == -4,
            "Normalization configuration did not round trip.");
        for (const auto& invalid : {
            "normalization_target_lufs=NaN", "normalization_target_lufs=-37",
            "normalization_unmeasured_fallback_db=12.5"
        }) {
            writeTuning(invalid);
            rejects([&] { readConfig(tuning); }, "Invalid analysis configuration was accepted.");
        }
        log << "PASS advanced analysis configuration: valid values, finite/range strictness\n";
        playbackStateTests(root, fixture, log);
        featureTests(root, fixture, log);
        log << "ALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}
