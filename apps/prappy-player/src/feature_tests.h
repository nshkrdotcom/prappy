#pragma once
#include "browse.h"
#include "playback.h"
#include "preferences.h"
#include "grouped_list.h"
#include <SDL3/SDL.h>

inline void featureTests(const fs::path& root, const Library& fixture, std::ostream& log) {
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    {
        const auto base = fs::path(L"C:\\Music");
        DirectoryFilter scope;
        DirectoryFilter::set(scope.excluded, base / "a", true);
        require(!scope.accepts(base / "a/nested/song.wav") && scope.accepts(base / "ab/song.wav"), "Subtree exclusion crossed a directory boundary.");
        DirectoryFilter::set(scope.included, base / "A", true);
        require(!scope.accepts(base / "a/song.wav"), "Include overrode an excluded ancestor.");
        DirectoryFilter::set(scope.included, base / "b", true);
        Track duplicate; duplicate.path = base / "a/song.wav"; duplicate.copies = {base / "b/copy.wav"};
        require(scope.playbackPath(duplicate) == duplicate.copies.front(), "Directory inclusion ignored an allowed duplicate alias.");
        Library indexed; indexed.roots = {base, base / "a"}; indexed.tracks = {duplicate, duplicate};
        indexed.tracks[1].path = base / "a/nested/other.flac"; indexed.tracks[1].copies.clear();
        DirectoryIndex index; index.rebuild(indexed); index.prioritize({base / "a/nested"});
        require(index.roots.size() == 1 && index.nodes.at(directoryKey(base)).count == 2 && index.nodes.at(directoryKey(base / "a")).count == 2 &&
            index.nodes.at(directoryKey(base / "b")).count == 1 && index.featured.front() == directoryKey(base / "a/nested"),
            "Directory priorities/counts lost aliases or double-counted tracks.");
        scope.only(base / "b"); require(scope.playbackPath(duplicate) == duplicate.copies.front() && !scope.accepts(base / "ab/x.wav"), "Directory-only selection leaked siblings.");
        Browse selected; selected.directories = scope;
        require(selected.select(indexed,"copy") == std::vector<size_t>({0}) && selected.select(indexed,"song").empty(),
            "Scoped search matched an excluded canonical filename or lost the allowed alias.");
        const fs::path second = L"D:\\Other"; indexed.roots.push_back(second);
        Track another; another.path = second / "priority/tone.wav"; indexed.tracks.push_back(another);
        index.rebuild(indexed); index.prioritize({});
        require(index.roots.size() == 2 && index.featured.front() == directoryKey(base / "a") &&
            std::find(index.featured.begin(),index.featured.end(),directoryKey(second / "priority")) != index.featured.end(),
            "Multiple roots did not prioritize their first-level directories.");
        require(inDirectory(directoryKey(L"\\\\?\\UNC\\server\\share\\Audio\\sub\\x.wav"), directoryKey(L"\\\\SERVER\\share\\audio\\")), "UNC/case directory matching failed.");
        require(TransportLayout::height(400,250,false) == 202 && TransportLayout::height(400,710,true) == 400 &&
            TransportLayout::resized(202,100,710,true) == 302 && TransportLayout::resized(202,-100,710,true) == 202,
            "Transport resize/clamp lost the saved expanded height.");
        log << "PASS directory scopes: subtree boundaries, exclusion precedence, multiple includes, aliases, unique counts, pins, UNC; transport resize/clamp\n";
    }
    {
        Library many; many.order = 1;
        std::vector<size_t> visible;
        for (size_t i = 0; i < 10000; ++i) {
            Track track; track.directory = "folder-" + std::to_string(i / 2); track.type = "wav";
            many.tracks.push_back(std::move(track)); visible.push_back(i);
        }
        GroupedList grouped; grouped.rebuild(many, visible); grouped.flatten(visible);
        require(grouped.groups.size() == 5000 && grouped.rows.size() == 15000 &&
            grouped.rows[0].header && !grouped.rows[1].header && grouped.rows[2].index == 1,
            "Grouped rows lost headers or track identity.");
        grouped.toggle(0, false); grouped.flatten(visible);
        require(grouped.rows.size() == 14998 && grouped.rows[1].header, "Collapsed group retained hidden tracks.");
        many.order = 2; grouped.rebuild(many, visible); grouped.flatten(visible);
        require(grouped.groups.size() == 1 && grouped.rows.size() == 10001, "File-type group inherited a directory collapse.");
        many.order = 1; grouped.rebuild(many, visible); grouped.flatten(visible);
        require(!grouped.groups[0].open && grouped.rows.size() == 14998, "Directory collapse was lost after sorting.");
        visible.erase(visible.begin(), visible.begin() + 2); grouped.rebuild(many, visible); grouped.flatten(visible);
        require(grouped.groups.front().key == "folder-1" && grouped.rows[1].index == 2, "Filtered groups retained stale indices.");
        log << "PASS grouped list: 10000 tracks, flattened headers, collapse, independent sort state and filtered identities\n";
    }
    require(wslLinuxPath(L"\\\\wsl.localhost\\Ubuntu\\home\\home\\a b.flac") == "/home/home/a b.flac", "WSL Linux conversion failed.");
    require(wslLinuxPath(L"\\\\WSL$\\Ubuntu\\home\\Beyonc\u00e9.flac") == "/home/Beyonc\xc3\xa9.flac", "WSL alias/Unicode conversion failed.");
    require(wslLinuxPath(L"\\\\?\\UNC\\wsl.localhost\\Ubuntu\\home\\music.wav") == "/home/music.wav", "Extended WSL conversion failed.");
    require(wslLinuxPath(L"\\\\wsl.localhost\\Ubuntu") == "/" && !wslLinuxPath(L"C:\\Music\\a.wav") &&
        !wslLinuxPath(L"\\\\server\\share\\a.wav"), "WSL conversion accepted a non-WSL share.");
    const auto& a = fixture.tracks[0].path; const auto& b = fixture.tracks[1].path;
    History queued; const auto aId = queued.add(a), bId = queued.add(b); queued.visit(0);
    const auto next = queued.enqueue(b, true), tail = queued.enqueue(a, false);
    require(queued.playedCount == 2 && queued.trail == std::vector<uint64_t>({aId, next, bId, tail}) && queued.find(next)->time == 0,
        "Queue insertion added a visit or lost forward history.");
    auto restored = History::parse(queued.serialize(), 10000);
    require(restored.playedCount == 2 && restored.find(next)->queued && restored.trail == queued.trail, "Queue persistence failed.");
    queued.add(a);
    require(queued.trail.size() == 4 && queued.trail[2] == next && queued.trail[3] == tail && queued.find(bId), "New branch discarded explicit queued tracks.");
    queued.visit(2); require(!queued.find(next)->queued && queued.playedCount == 4 && queued.find(next)->time > 0, "Queued playback did not become one successful visit.");
    queued.removeQueued(tail); require(!queued.find(tail) && queued.current()->id == next, "Queue removal moved the current visit.");
    queued.setLimit(1); queued.enqueue(a, false);
    require(queued.playedCount == 1 && queued.current()->id == next && queued.entries.size() == 2, "History retention erased pending queue.");
    History idle; idle.enqueue(a, false); idle.enqueue(b, false);
    require(!idle.current() && !History::parse(idle.serialize(), 10000).current(), "Idle queue restored as a played visit.");
    History retention; retention.setLimit(2);
    retention.add(a); retention.entries[0].time = 10;
    const auto planned = retention.enqueue(b, false);
    retention.add(a); retention.entries[2].time = 20;
    retention.visit(retention.trail.size() - 1);
    const auto recent = retention.current()->id; require(recent == planned, "Retention queue fixture order is wrong.");
    retention.add(a);
    require(retention.find(recent) && retention.playedCount == 2, "Retention used queue insertion time instead of successful playback time.");

    Library library = fixture;
    Track extra = library.tracks.front(); extra.path = root / "other" / "new.flac"; extra.id = key(extra.path);
    extra.label = "other/new.flac"; extra.name = "new.flac"; extra.type = "flac"; extra.copies.clear(); extra.offline = true;
    library.tracks.push_back(extra); library.tracks[0].duration = 30; library.tracks[1].duration = 90;
    library.rebuildSearch();
    History visits; visits.add(a); visits.add(a); visits.add(b);
    Browse browse; browse.rebuild(library, visits);
    browse.view = Browse::View::Unplayed;
    require(browse.select(library, "") == std::vector<size_t>({2}), "Unplayed view failed.");
    browse.view = Browse::View::MostPlayed;
    require(browse.select(library, "").front() == 0 && browse.played(library.tracks[0]).count == 2, "Most played view failed.");
    auto& entries = visits.entries; entries[0].time = entries[1].time = 100; entries[2].time = 200;
    browse.rebuild(library, visits); browse.view = Browse::View::LastPlayed;
    require(browse.select(library, "").front() == 1, "Last played view failed.");
    library.tracks[0].copies.push_back(root / "alias.wav"); visits.add(root / "alias.wav"); visits.enqueue(extra.path, false);
    browse.rebuild(library, visits); require(browse.played(library.tracks[0]).count == 3 && browse.played(extra).count == 0, "Stats lost duplicate identity or counted queued paths.");
    browse.view = Browse::View::All; browse.availability = 2;
    require(browse.select(library, "") == std::vector<size_t>({2}), "Offline filter failed.");
    browse.availability = 0; browse.extension = "flac";
    require(browse.select(library, "") == std::vector<size_t>({2}), "Extension filter failed.");
    browse.extension.clear(); browse.minimum = 40; browse.maximum = 100;
    require(browse.select(library, "") == std::vector<size_t>({1}), "Duration bounds/unknown handling failed.");
    library.order = 3; library.sort(); require(library.tracks.back().id == extra.id && *library.tracks[0].duration == 30, "Duration sorting failed.");
    Library ranked;
    Track exact = extra; exact.name = "night.wav"; exact.label = "library/night.wav"; exact.id = L"exact";
    Track directory = extra; directory.name = "other.wav"; directory.label = "night/other.wav"; directory.id = L"directory";
    Track alias = extra; alias.name = "third.wav"; alias.label = "third.wav"; alias.id = L"alias"; alias.copies = {root / "night.wav"};
    ranked.tracks = {alias, directory, exact}; ranked.rebuildSearch(); Browse ranking;
    require(ranking.select(ranked, "NIGHT") == std::vector<size_t>({2, 1, 0}) && ranking.select(ranked, "night missing").empty(), "Ranked filename/path/alias search failed.");

    analysis::Summary measured; measured.loudness = -24; measured.samplePeak = .1f;
    require(std::abs(normalizationGain(&measured, true) - 6) < .001 && normalizationGain(&measured, false) == 0, "Normalization target/disable failed.");
    measured.loudness = -40; require(normalizationGain(&measured, true) == 12, "Normalization boost cap failed.");
    measured.samplePeak = 1; require(std::abs(normalizationGain(&measured, true) + 1) < .001, "Normalization peak headroom failed.");
    measured.loudness.reset(); require(normalizationGain(&measured, true) == 0 && normalizationGain(nullptr, true) == 0, "Unknown loudness received gain.");
    Preferences preferences; preferences.topmost = preferences.strip = preferences.normalize = true; preferences.folder = a.parent_path();
    preferences.transportHeight = 390; preferences.hiddenDirectories = {a.parent_path()}; preferences.pinnedDirectories = {b.parent_path()};
    const auto prefs = root / "preferences.txt"; stateio::write(prefs, preferences.serialize());
    const auto loaded = Preferences::load(prefs);
    require(loaded.topmost && loaded.strip && loaded.normalize && loaded.folder == preferences.folder, "Window/normalization preferences did not round trip.");
    require(loaded.transportHeight == 390 && loaded.hiddenDirectories == preferences.hiddenDirectories && loaded.pinnedDirectories == preferences.pinnedDirectories,
        "Directory/panel preferences did not round trip.");
    log << "PASS features model: WSL paths, queue/forward trail, retained-history views, ranked search, duration/type/offline filters, gain/headroom, preferences\n" << std::flush;

    Audio audio; audio.open(true); History history; HistoryFile file(root / "feature-playback.txt"); Playback player(audio, library, history, file);
    const auto wait = [&] {
        for (int i = 0; i < 1000; ++i) { player.tick(); if (audio.loaded && !player.pending) return; SDL_Delay(5); }
        throw std::runtime_error("Feature playback timeout: " + player.notice + " " + audio.error);
    };
    player.enqueue(a, false); player.enqueue(b, false);
    require(player.navigate(1) && player.navigate(1), "Idle queued playback failed."); wait();
    require(history.playedCount == 1 && key(audio.current) == key(a), "Rapid Next skipped an uncommitted queued track.");
    for (int i = 0; i < 40 && audio.meterPeak <= 0; ++i) { SDL_Delay(5); player.tick(); }
    require(audio.meterPeak > .01 && audio.meterPeak < .1, "Live decoded PCM peak meter returned no signal.");
    const auto volume = audio.volume; audio.setGain(-6);
    double applied = 0; audio.get_property(audio.handle, "volume-gain", MPV_FORMAT_DOUBLE, &applied);
    require(std::abs(applied + 6) < .001 && audio.volume == volume, "Measured gain changed user volume or was not applied.");
    require(player.navigate(1), "Second queued track failed."); wait(); require(history.playedCount == 2 && key(audio.current) == key(b), "Queue consumption committed the wrong visit.");
    const auto missing = root / "queued-missing.wav"; player.enqueue(missing, true); const auto oldId = history.current()->id;
    require(player.navigate(1), "Missing queued path request failed."); wait();
    require(history.current()->id == oldId && history.find(history.trail[history.cursor + 1])->queued && history.playedCount == 2,
        "Failed queue load consumed the planned visit or damaged history.");
    history.removeQueued(history.trail[history.cursor + 1]);
    player.enqueue(a, true); const auto cancelled = history.trail[history.cursor + 1];
    require(player.navigate(1), "Cannot start queued load for removal test."); player.removeQueued(cancelled); wait();
    require(!history.find(cancelled) && history.current()->id == oldId && history.playedCount == 2,
        "Removing an in-flight queued load created a ghost visit.");
    require(player.playFolder(a.parent_path(), true), "Folder playback failed."); wait();
    require(player.folder == a.parent_path() && history.repeat == RepeatMode::All && key(audio.current.parent_path()) == key(player.folder), "Folder scope/repeat failed.");
    require(player.libraryNext(true).parent_path() == player.folder, "Folder progression escaped its directory.");
    require(!player.playFolder(root / "empty", false) && player.folder == a.parent_path(), "Empty folder corrupted playback scope.");
    fs::copy_file(a, root / "alias.wav", fs::copy_options::overwrite_existing);
    require(player.playFolder(root, true), "Folder containing a hidden duplicate was not playable."); wait();
    require(audio.current == root / "alias.wav" && player.libraryNext(true) == audio.current,
        "Folder progression lost the folder's duplicate-copy path.");
    player.toggle(); player.tick(); require(audio.paused && audio.meterPeak == 0, "Paused peak meter did not clear.");
    player.finish(); log << "PASS features playback: idle/rapid queue, successful consumption, failed-load rollback, live PCM meter, independent gain, folder scope/repeat\n";
}
