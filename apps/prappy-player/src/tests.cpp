#include "tests.h"
#include "analysis_tests.h"
#include "playback.h"
#include "scan.h"
#include "status.h"
#include "feature_tests.h"
#include "text.h"
#include "ui_schedule.h"
#include "waveform_tests.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <fstream>
// Integration checks run only in explicit --state-test fixture directories.
int stateTests(const fs::path& configPath, const fs::path& logPath) {
    std::ofstream log(logPath);
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto rejects = [&](const auto& operation, const char* message) {
        bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
        require(rejected, message);
    };
    try {
        waveformRenderTests(log);
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
            require(std::abs(approachGain(0, 12, .5) - 4.5) < .001 &&
                std::abs(releasePeak(releasePeak(1, 0, .1), 0, .2) - releasePeak(1, 0, .3)) < .00001,
                "Gain or meter depends on frame rate.");
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
            log << "PASS asynchronous seek coalescing, immediate preview, smooth clock and paused marker\n";
        }
        require(statusText("", "Checking duplicates: 4 / 10") == "Checking duplicates: 4 / 10", "Empty status adds a leading separator.");
        require(statusText("10 tracks | 2 identical copies hidden", "Checking duplicates: 4 / 10") ==
            "10 tracks \xc2\xb7 2 identical copies hidden \xc2\xb7 Checking duplicates: 4 / 10", "Status sections are not joined with middle dots.");
        const auto root = logPath.parent_path() / ("state-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(root);
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

        Library fixture; require(fixture.refresh(configPath) && fixture.tracks.size() >= 2, "State tests need two valid distinct tracks.");
        const auto media = root / "media"; fs::create_directories(media);
        const auto first = media / "a.wav", copy = media / "b.wav", other = media / "c.wav";
        fs::copy_file(fixture.tracks[0].path, first, fs::copy_options::overwrite_existing);
        fs::copy_file(fixture.tracks[0].path, copy, fs::copy_options::overwrite_existing);
        fs::copy_file(fixture.tracks[1].path, other, fs::copy_options::overwrite_existing);
        const auto localConfig = root / "player.ini";
        { std::ofstream config(localConfig); config << "directory=" << utf8(media) << "\nextensions=wav\nhistory_limit=23\n"; }
        auto settings = readConfig(localConfig); require(settings.historyLimit == 23, "Configured history limit failed.");
        const auto badConfig = root / "invalid.ini";
        { std::ofstream config(badConfig); config << "directory=" << utf8(media) << "\nextensions=wav\nhistory_limit=-1\n"; }
        rejects([&] { readConfig(badConfig); }, "Invalid history limit accepted.");
        Library catalog; ScanProgress initial;
        require(catalog.refresh(localConfig, {}, &initial) && catalog.tracks.size() == 2 && initial.hashReads > 0, "Catalog initial duplicate verification failed.");
        const auto catalogPath = root / "catalog.bin"; saveCatalog(catalogPath, catalog);
        Library cached; settings.historyLimit = 99;
        require(loadCatalog(catalogPath, settings, cached) && cached.tracks.size() == 2 && cached.historyLimit == 99, "Catalog cache incorrectly depends on session settings.");
        ScanProgress reused;
        require(cached.refresh(localConfig, {}, &reused) && reused.hashReads == 0 && reused.hashReused > 0, "Unchanged files were rehashed.");
        auto bytes = catalogBytes(catalog); bytes.back() ^= 1;
        rejects([&] { parseCatalog(bytes, settings); }, "Catalog checksum damage accepted.");
        bytes = catalogBytes(catalog); bytes[8] = 2;
        rejects([&] { parseCatalog(bytes, settings); }, "Incompatible catalog accepted.");
        auto different = settings; different.extensions.insert(".mp3");
        rejects([&] { parseCatalog(catalogBytes(catalog), different); }, "Changed catalog configuration accepted.");
        { std::fstream changed(copy, std::ios::in | std::ios::out | std::ios::binary); changed.seekg(16384); char byte = 0; changed.read(&byte, 1); byte ^= 2; changed.seekp(16384); changed.write(&byte, 1); }
        fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::seconds(2));
        ScanProgress changed;
        require(cached.refresh(localConfig, {}, &changed) && cached.tracks.size() == 3 && changed.hashReads > 0, "Changed content reused stale verification.");
        cached.verified.clear(); cached.forceVerification = true; ScanProgress forced;
        require(cached.refresh(localConfig, {}, &forced) && forced.hashReads > 0, "Forced duplicate revalidation was skipped.");
        fs::remove(copy);
        require(cached.refresh(localConfig) && cached.tracks.size() == 2, "Deleted file retained while root online.");
        fs::create_hard_link(first, media / "hard-link.wav");
        require(cached.refresh(localConfig) && cached.tracks.size() == 2, "Hard-link fixture was not grouped.");
        fs::rename(media, root / "offline-media");
        require(cached.refresh(localConfig) && cached.tracks.size() == 2 && std::all_of(cached.tracks.begin(), cached.tracks.end(), [](const auto& track) { return track.offline; }), "Offline root lost catalog entries.");
        fs::rename(root / "offline-media", media);
        require(cached.refresh(localConfig) && std::none_of(cached.tracks.begin(), cached.tracks.end(), [](const auto& track) { return track.offline; }), "Returning root stayed offline.");
        auto aliased = cached;
        const auto missingAlias = root / "missing-configured-alias";
        for (auto& file : aliased.files) file.sourceRoot = missingAlias;
        aliased.roots = {missingAlias};
        const auto aliasConfig = root / "alias.ini";
        { std::ofstream config(aliasConfig); config << "directory=" << utf8(missingAlias) << "\nextensions=wav\n"; }
        require(aliased.refresh(aliasConfig) && aliased.tracks.size() == cached.tracks.size(), "Offline configured alias lost canonical target paths.");
        stateio::write(catalogPath, catalogBytes(cached));
        { std::ofstream corrupt(catalogPath, std::ios::binary); corrupt << "bad"; }
        require(loadCatalog(catalogPath, settings, catalog), "Catalog backup recovery failed.");
        log << "PASS catalog: independent version, checksum, config matching, unchanged-hash reuse, changed/deleted files, offline roots, backup recovery\n";

        Audio audio; audio.open(true);
        History visits; HistoryFile sessionFile(root / "session.txt"); Playback player(audio, fixture, visits, sessionFile);
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
        for (int i = 0; i < 400 && (audio.info.codec.empty() || audio.info.sampleFormat.empty() || audio.info.sampleRate <= 0 || audio.info.channels <= 0); ++i) {
            SDL_Delay(5); player.tick();
        }
        require(audio.info.codec == "pcm_s16le" && audio.info.sampleRate == 44100 && audio.info.channels == 1 &&
            !audio.info.sampleFormat.empty() && audio.info.describe().find("44100 Hz") != std::string::npos,
            "Source audio format details are missing or incorrect.");
        play(fixture.tracks[1].path); const auto visitB = visits.current()->id;
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
        require(player.request(root / "missing.wav"), "Missing request not accepted by mpv."); wait(player, audio);
        require(visits.entries.size() == beforeMissing && visits.current()->id == currentId && !player.notice.empty(), "Failed load changed history or lost error.");
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
        for (int i = 0; i < 200 && std::abs(audio.position - .8) > .25; ++i) { player.tick(); SDL_Delay(5); }
        require(audio.paused && audio.position > .5, "Cannot checkpoint paused position.");
        double decoded = 0;
        require(audio.get_property(audio.handle,"time-pos",MPV_FORMAT_DOUBLE,&decoded) >= 0 && std::abs(decoded-.8) < .25,
            "Seek preview hid an incorrect decoder position.");
        player.checkpoint(); visits.repeat = RepeatMode::One; ++visits.revision; player.finish();
        HistoryFile restoredFile(root / "session.txt"); auto restoredHistory = restoredFile.load(10000);
        Audio restoredAudio; restoredAudio.open(true); Playback restored(restoredAudio, fixture, restoredHistory, restoredFile);
        restored.restore(); wait(restored, restoredAudio);
        require(restoredAudio.paused && restoredHistory.entries.size() == visits.entries.size() && restoredAudio.position > .5 && restoredHistory.repeat == RepeatMode::One, "Session did not restore paused without adding visits.");
        int paused = 0; restoredAudio.get_property(restoredAudio.handle, "pause", MPV_FORMAT_FLAG, &paused);
        require(paused != 0, "Restored mpv was not paused.");
        log << "PASS playback: branch/Back/Forward, rapid requests, failed-load rollback, paused session/position/repeat restore\n";

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
        HistoryFile oneFile(root / "one.txt"); Audio oneAudio; oneAudio.open(true); Playback onePlayer(oneAudio, one, oneHistory, oneFile);
        require(onePlayer.navigate(1), "Single-track start failed."); wait(onePlayer, oneAudio);
        oneAudio.seek(oneAudio.duration - .12); const auto beforeOne = oneAudio.serial;
        awaitEofAction(onePlayer, oneAudio, beforeOne);
        require(oneHistory.entries.size() == 2 && key(oneAudio.current) == one.tracks.front().id, "Single-track Repeat All failed.");
        Library empty; History noHistory; HistoryFile noFile(root / "empty.txt"); Playback noPlayer(oneAudio, empty, noHistory, noFile);
        require(!noPlayer.navigate(1), "Empty library should not start/retry playback.");
        restored.finish(); onePlayer.finish(); player.finish();
        log << "PASS repeat: One preserves visits/manual navigation; All wraps; Off stops; one-track and empty libraries\n";
        analysisTests(root, fixture, log);
        featureTests(root, fixture, log);
        log << "ALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}
int selfTest(const fs::path& config, const fs::path& logFile) {
    std::ofstream log(logFile);
    try {
        Library searchLibrary;
        Track lead; lead.label = "library\\Jazz\\Night Bus.flac"; lead.name = "Night Bus.flac";
        lead.directory = "Jazz"; lead.type = "flac"; lead.id = L"night";
        lead.copies = {L"C:/scraper/library/Alternate Take.opus"};
        Track other; other.label = "library\\Jazz\\Morning.flac"; other.name = "Morning.flac";
        other.directory = "Jazz"; other.type = "flac"; other.id = L"morning";
        Track unicode; unicode.label = "library\\Beyoncé\\Live.m4a"; unicode.name = "Live.m4a";
        unicode.directory = "Beyoncé"; unicode.type = "m4a"; unicode.id = L"live";
        searchLibrary.tracks = {lead, other, unicode};
        searchLibrary.rebuildSearch();
        for (int order = 0; order < 3; ++order) {
            searchLibrary.order = order; searchLibrary.sort();
            for (const char* query : {"NIGHT jazz", "jazz/night", "alternate OPUS"}) {
                const auto matches = Browse{}.select(searchLibrary, query);
                if (matches.size() != 1 || searchLibrary.tracks[matches[0]].id != L"night")
                    throw std::runtime_error("Search lost track identity or failed word/path/alias matching.");
            }
        }
        if (Browse{}.select(searchLibrary, "BEYONCÉ").size() != 1 || Browse{}.select(searchLibrary, "  ").size() != 3 ||
            !Browse{}.select(searchLibrary, "no-such-track").empty() || searchLibrary.tracks.size() != 3)
            throw std::runtime_error("Search failed Unicode, clear or no-match behavior.");
        log << "PASS search: words / case / Unicode / paths / copies / sorting / clear / no matches\n";
        Library library;
        if (!library.refresh(config) || library.tracks.empty()) throw std::runtime_error(library.notice);
        if (library.tracks.size() < 2) throw std::runtime_error("Self-test needs at least two tracks.");
        log << "Scan: " << library.notice << '\n';
        Audio audio; audio.open(true);
        std::set<std::string> formats;
        const auto testTracks = library.tracks;
        for (auto track : testTracks) {
            if (!formats.insert(track.type).second) continue;
            log << "Testing " << track.label << std::endl;
            if (!audio.play(track.path)) throw std::runtime_error(audio.error);
            auto waitLoaded = [&] {
                for (int i = 0; i < 500; ++i) { audio.poll(); if (audio.loaded) return; if (!audio.error.empty()) throw std::runtime_error(audio.error); SDL_Delay(10); }
                throw std::runtime_error("Playback load timeout: " + track.name);
            };
            waitLoaded(); SDL_Delay(120); audio.poll();
            if (audio.duration <= 0) throw std::runtime_error("No duration: " + track.name);
            audio.pause(); audio.poll();
            int paused = 0; audio.get_property(audio.handle, "pause", MPV_FORMAT_FLAG, &paused);
            if (!paused) throw std::runtime_error("Pause failed.");
            double seek = std::min(1.0, audio.duration / 2); audio.seek(seek);
            for (int i = 0; i < 100; ++i) {
                SDL_Delay(10); audio.poll();
                if (std::abs(audio.position - seek) <= .25) break;
            }
            // mpv reports audible position, including the paused audio output buffer.
            if (std::abs(audio.position - seek) > .25) throw std::runtime_error("Seek failed: wanted " + std::to_string(seek) + ", got " + std::to_string(audio.position) + ": " + audio.error);
            audio.setVolume(37); double volume = 0; audio.get_property(audio.handle, "volume", MPV_FORMAT_DOUBLE, &volume);
            if (std::abs(volume - 37) > .1) throw std::runtime_error("Volume failed.");
            auto id = key(audio.current);
            for (int order = 0; order < 3; ++order) { library.order = order; library.sort(); if (key(audio.current) != id) throw std::runtime_error("Sort changed current track."); }
            if (!library.refresh(config) || key(audio.current) != id) throw std::runtime_error("Refresh changed current track.");
            audio.pause(); log << "PASS " << track.type << ": decode / duration / pause / seek / volume / sort / refresh\n";
        }
        History navigation; HistoryFile navigationFile(config.parent_path() / "selftest-history.txt");
        Playback playback(audio, library, navigation, navigationFile);
        const auto waitPlayback = [&] {
            for (int i = 0; i < 1000; ++i) {
                playback.tick();
                if (audio.loaded && !playback.pending) return;
                SDL_Delay(5);
            }
            throw std::runtime_error("Navigation timeout: " + playback.notice);
        };
        if (!playback.request(audio.current)) throw std::runtime_error("Cannot initialize navigation.");
        waitPlayback();
        for (const auto& track : testTracks) {
            if (track.copies.empty()) continue;
            const auto copy = track.copies.front();
            playback.request(copy); waitPlayback();
            if (!library.refresh(config)) throw std::runtime_error("Alias refresh failed.");
            const auto id = key(copy);
            auto current = std::find_if(library.tracks.begin(), library.tracks.end(), [&](auto& t) { return Library::matches(t, id); });
            if (current == library.tracks.end()) throw std::runtime_error("Refresh lost alternate-copy identity.");
            auto nextIndex = (static_cast<size_t>(current - library.tracks.begin()) + 1) % library.tracks.size();
            if (!playback.navigate(1) || !Library::matches(library.tracks[nextIndex], key(audio.current)))
                throw std::runtime_error("Next from alternate copy failed.");
            waitPlayback();
            log << "PASS alternate-copy identity across refresh and next\n";
            break;
        }
        if (!playback.navigate(1)) throw std::runtime_error("Next failed.");
        waitPlayback();
        auto next = key(audio.current);
        if (!playback.navigate(-1) || key(audio.current) == next) throw std::runtime_error("Previous failed.");
        waitPlayback();
        log << "PASS next / previous\n";
        library.order = 0; library.sort();
        playback.request(library.tracks.front().path); waitPlayback();
        audio.seek(std::max(0.0, audio.duration - .15));
        const auto beforeEof = audio.serial;
        for (int i = 0; i < 500 && audio.serial == beforeEof; ++i) { playback.tick(); SDL_Delay(10); }
        if (audio.serial == beforeEof || key(audio.current) != library.tracks[1].id)
            throw std::runtime_error("Automatic progression failed.");
        waitPlayback();
        log << "PASS end-of-track progression\n";
        audio.play(config.parent_path() / L"missing-audio.flac");
        for (int i = 0; i < 300 && audio.error.empty(); ++i) { audio.poll(); SDL_Delay(10); }
        if (audio.error.empty()) throw std::runtime_error("Playback error was not surfaced.");
        log << "PASS visible playback error state: " << audio.error << '\n';
        // Check a real Windows output without producing a test tone in the room.
        Audio output; output.open(); output.setVolume(0); output.play(testTracks.front().path);
        std::string outputDriver;
        for (int i = 0; i < 300; ++i) {
            output.poll();
            char* driver = nullptr;
            if (output.get_property(output.handle, "current-ao", MPV_FORMAT_STRING, &driver) >= 0 && driver) {
                outputDriver = driver; output.free(driver);
            }
            if (!outputDriver.empty()) break;
            if (!output.error.empty()) throw std::runtime_error(output.error);
            SDL_Delay(10);
        }
        if (outputDriver.empty() || outputDriver == "null") throw std::runtime_error("Windows audio output did not initialize.");
        log << "PASS Windows audio output initialized (muted): " << outputDriver << '\n';
        const auto id = key(audio.current);
        const auto count = library.tracks.size();
        if (library.refresh(logFile.parent_path() / "nonexistent-player-config.ini") || library.tracks.size() != count || key(audio.current) != id)
            throw std::runtime_error("Bad config did not preserve playback/library.");
        log << "PASS missing config preserves library and playback\n";
        for (int cycle = 0; cycle < 12; ++cycle) {
            Audio repeated; repeated.open(true);
            if (!repeated.play(testTracks[static_cast<size_t>(cycle) % testTracks.size()].path))
                throw std::runtime_error(repeated.error);
            for (int i = 0; i < 300 && !repeated.loaded; ++i) {
                repeated.poll();
                if (!repeated.error.empty()) throw std::runtime_error(repeated.error);
                SDL_Delay(10);
            }
            if (!repeated.loaded) throw std::runtime_error("Repeated playback load timeout.");
            if (!library.refresh(config)) throw std::runtime_error("Repeated refresh failed.");
            Scan cancelled; cancelled.begin(config, cycle % 3);
            // Destruction requests cancellation and joins even while a scan is active.
        }
        log << "PASS 12 playback create/load/destroy, refresh and scan cancellation cycles\nALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}
