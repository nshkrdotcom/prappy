#pragma once
// Integration checks run only in explicit --state-test fixture directories.
static int stateTests(const fs::path& configPath, const fs::path& logPath) {
    std::ofstream log(logPath);
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto rejects = [&](const auto& operation, const char* message) {
        bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
        require(rejected, message);
    };
    try {
        const auto root = logPath.parent_path() / ("state-test-" + std::to_string(GetCurrentProcessId()));
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
        audio.pause(); audio.seek(.8);
        for (int i = 0; i < 200 && std::abs(audio.position - .8) > .25; ++i) { player.tick(); SDL_Delay(5); }
        require(audio.paused && audio.position > .5, "Cannot checkpoint paused position.");
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
        log << "PASS repeat: One preserves visits/manual navigation; All wraps; Off stops; one-track and empty libraries\nALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}
