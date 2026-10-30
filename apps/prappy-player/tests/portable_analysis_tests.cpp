#include "gain_policy.h"
#include "analysis_registry.h"
#include "analysis_queue.h"
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
    check(gain::choose(20, "unknown-peak", enabled, positiveTag, {}).appliedDb == 0,
          "Positive tag gain with unknown peak must be capped");
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
static void registryTests() {
    namespace ar = analysis_registry;
    ar::Entries entries;
    for (size_t i = 0; i < 7159; ++i) {
        ar::Entry entry;
        entry.path = "/music/track" + std::to_string(i) + ".flac";
        entry.bytes = 50000 + i; entry.stamp = 90000 + i;
        entry.state = ar::State::Ready;
        entry.frames = 48000 * 180; entry.duration = 180;
        entry.loudness = -21.4; entry.samplePeak = 0.3;
        entry.waveform = ar::blobName(entry);
        entries.emplace(entry.path, std::move(entry));
    }
    // A probed track tag and its distinct reference survive persistence.
    auto& tagged = entries.begin()->second;
    tagged.tagsProbed = true;
    tagged.tags = gain::parseTags({{"R128_TRACK_GAIN", "-512"}}, true);
    auto encoded = ar::encode(entries);
    check(encoded.size() < 5*1024*1024, "Registry not compact");
    auto restored = ar::decode(encoded);
    check(restored.size() == entries.size() && restored.begin()->second.waveform == entries.begin()->second.waveform,
          "Registry round-trip");
    check(restored.begin()->second.tagsProbed && restored.begin()->second.tags.selected &&
          restored.begin()->second.tags.selected->origin == gain::Origin::R128 &&
          restored.begin()->second.tags.selected->referenceLufs == -23, "Tag provenance lost on restart");
    auto future = entries;
    future.begin()->second.algorithm = ar::Algorithm + 1;
    rejects([&] { ar::decode(ar::encode(future)); }, "Future analysis algorithm was not preserved");
    auto broken = encoded; broken.back() ^= 0x44;
    rejects([&] { ar::decode(broken); }, "Damaged registry checksum accepted");
    broken = encoded; broken[8] = 3;
    rejects([&] { ar::decode(broken); }, "Unknown registry version accepted");
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "prappy-portable-test-registry";
    fs::create_directories(root);
    try {
        ar::atomicWrite(root / "summaries.bin", encoded);
        auto reopened = ar::decode(ar::readFile(root / "summaries.bin"));
        check(reopened.size() == entries.size(), "Restart lost registry summaries");
        // There are intentionally NO waveform blobs at all: opening the
        // registry, including 7159 entries, cannot depend on waveform reads.
        check(!fs::exists(root / reopened.begin()->second.waveform), "Unexpected waveform artifact");
        ar::atomicWrite(root / "summaries.bin", encoded);
        check(ar::decode(ar::readFile(root / "summaries.bin")).size() == entries.size(), "Atomic rewrite failed");
    } catch (...) { fs::remove_all(root); throw; }
    fs::remove_all(root);
    std::cout << "PASS registry: 7159 ready records, restart without blobs, atomic rewrite, version/checksum guards\n";
}
static void schedulerTests() {
    using namespace analysis_queue;
    check(automaticWorkers(0) == 1 && automaticWorkers(1) == 1 && automaticWorkers(2) == 1 &&
          automaticWorkers(3) == 1 && automaticWorkers(64) == 4, "Worker auto limit");
    Queue queue;
    Job a{"a", 1, 1, Priority::Background, true}, b{"b", 1, 1, Priority::Background, true};
    Job c{"c", 1, 1, Priority::Selected, true};
    queue.offer(a); queue.offer(b);
    auto runA = queue.take(2); auto runB = queue.take(2);
    check(runA && runB && queue.running() == 2 && queue.remoteRunning() == 2, "Worker/remote saturation");
    queue.offer(c); check(!queue.hasRunnable(2), "Remote limit ignored");
    check(queue.preemptFor(c, 2, 2) && (runA->second.stop_requested() || runB->second.stop_requested()),
          "Foreground did not cancel background at saturation");
    auto victim = runA->second.stop_requested() ? runA->first : runB->first;
    queue.complete(victim, true, true);
    auto admitted = queue.take(2);
    check(admitted && admitted->first.key == "c" && queue.running() == 2,
          "Foreground was not admitted immediately after cancelled slot released");
    Queue one;
    one.offer(a); auto running = one.take(1);
    one.offer(c); check(one.preemptFor(c,1,1) && running->second.stop_requested(), "Single-worker preemption");
    one.complete(running->first, true, true);
    auto foreground = one.take(1); check(foreground && foreground->first.key == "c", "Single-worker admission");
    Queue dup;
    dup.offer(a); dup.offer(a); check(dup.waiting() == 1, "Pending dedup");
    dup.offer(Job{"a", 1, 1, Priority::Selected, true});
    check(dup.take(2)->first.priority == Priority::Selected, "Priority upgrade");
    Queue changed;
    changed.offer(a);
    changed.offer(Job{"a", 2, 2, Priority::Background, true});
    check(changed.waiting() == 1 && changed.take(1)->first.bytes == 2,
          "A changed source identity was not invalidated");
    Queue removed;
    removed.offer(a); removed.offer(b);
    removed.retain({"b"});
    check(removed.waiting() == 1 && removed.take(1)->first.key == "b",
          "Incremental offline/remove reconciliation retained stale work");
    // A fake blocking decoder, synchronized by barriers and stop-aware
    // condition variables, proves admission without waiting for any whole
    // background job to complete. No arbitrary scheduling sleeps.
    Queue concurrent;
    concurrent.offer(a); concurrent.offer(b);
    auto first = concurrent.take(2), second = concurrent.take(2);
    check(first && second, "Fake decoder setup");
    std::mutex stateMutex, queueMutex;
    std::condition_variable stateWake;
    std::condition_variable_any decoderWake;
    std::barrier arrived(3);
    int stopped = 0;
    auto launch = [&](std::pair<Job,std::stop_token> acquired) {
        return std::jthread([&, acquired](std::stop_token) {
            arrived.arrive_and_wait();
            std::mutex decoderMutex;
            std::unique_lock decoderLock(decoderMutex);
            decoderWake.wait(decoderLock, acquired.second, [] { return false; });
            {
                std::lock_guard queueLock(queueMutex);
                concurrent.complete(acquired.first, true, true);
            }
            {
                std::lock_guard stateLock(stateMutex);
                ++stopped;
            }
            stateWake.notify_all();
        });
    };
    auto threadA = launch(*first), threadB = launch(*second);
    arrived.arrive_and_wait();
    {
        std::lock_guard queueLock(queueMutex);
        concurrent.offer(c);
        check(concurrent.preemptFor(c,2,2), "Fake decoder preemption not initiated");
    }
    {
        std::unique_lock stateLock(stateMutex);
        check(stateWake.wait_for(stateLock, 5s, [&] { return stopped > 0; }),
              "Cancellable background decoder failed to yield its slot");
    }
    {
        std::lock_guard queueLock(queueMutex);
        auto foregroundJob = concurrent.take(2);
        check(foregroundJob && foregroundJob->first.key == "c", "Foreground missed freed slot");
        concurrent.cancelAll();
    }
    decoderWake.notify_all();
    threadA.join(); threadB.join();
    std::cout << "PASS scheduler: 1/2 workers, remote saturation, preemption, fake blocked decoders, dedup, upgrades\n";
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
    try { gainTests(); registryTests(); schedulerTests(); dspTests(); }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    std::cout << "ALL PORTABLE ANALYSIS TESTS PASS\n";
}
