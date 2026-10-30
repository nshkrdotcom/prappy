#include "analysis_service.h"
#include "library.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <thread>

namespace analysis {
namespace {
using Clock = std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point begin) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count());
}
uint64_t wallSeconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string idOf(const Identity& identity) { return analysis_registry::pathUtf8(identity.path); }
bool isRemote(const fs::path& path) {
    const auto value = key(path);
    return value.starts_with(L"\\\\wsl$\\") || value.starts_with(L"\\\\wsl.localhost\\") ||
        value.starts_with(L"\\\\?\\unc\\wsl$\\") || value.starts_with(L"\\\\?\\unc\\wsl.localhost\\");
}
analysis_queue::Job makeJob(const Identity& id, analysis_queue::Priority priority, bool force) {
    return {idOf(id), id.bytes, id.stamp, priority, isRemote(id.path), force};
}
float peakOf(const Result& result) {
    const auto& peak = result.levels.back().front();
    return std::max({std::abs(peak.leftMin), std::abs(peak.leftMax), std::abs(peak.rightMin), std::abs(peak.rightMax)});
}
size_t waveformBytes(const Result& result) {
    size_t bytes = sizeof(Result);
    for (const auto& level : result.levels) bytes += level.capacity() * sizeof(Peak);
    return bytes;
}
}
Service::Service(fs::path cacheDirectory, ServiceSettings limits, std::function<void()> wakeUI)
    : directory(std::move(cacheDirectory)), settings(limits), notify(std::move(wakeUI)) {
    if (settings.remoteLimit < 1 || settings.remoteLimit > 4 ||
        (settings.workers > 4) || settings.waveformMemoryMiB < 16 || settings.waveformMemoryMiB > 512)
        throw std::invalid_argument("Invalid analysis service limits");
    maxWaveBytes = static_cast<size_t>(settings.waveformMemoryMiB) * 1024 * 1024;
    const auto count = settings.workers ? settings.workers : analysis_queue::automaticWorkers(std::thread::hardware_concurrency());
    // The threads inspect workers under this mutex. Publish the entire vector
    // before either loop can read it, including an immediately loaded registry.
    std::lock_guard lock(mutex);
    ioThread = std::jthread([this](std::stop_token stop) { ioLoop(stop); });
    for (unsigned i = 0; i < count; ++i)
        workers.emplace_back([this](std::stop_token stop) { decodeLoop(stop); });
}
Service::~Service() {
    // Stop decode/pipe operations *before* the sole registry writer flushes.
    for (auto& thread : workers) thread.request_stop();
    { std::lock_guard lock(mutex); queue.cancelAll(); }
    wake.notify_all();
    for (auto& thread : workers) if (thread.joinable()) thread.join();
    ioThread.request_stop(); wake.notify_all();
    if (ioThread.joinable()) ioThread.join();
}
bool Service::matching(const analysis_registry::Entry& entry, const Identity& id) {
    return entry.path == idOf(id) && entry.bytes == id.bytes && entry.stamp == id.stamp &&
        entry.algorithm == analysis_registry::Algorithm;
}
Summary Service::summaryFor(const analysis_registry::Entry& entry, const Identity& id) {
    Summary value; value.identity = id; value.duration = entry.duration;
    value.loudness = entry.loudness; value.samplePeak = static_cast<float>(entry.samplePeak.value_or(0));
    value.error = entry.error; value.cached = true; return value;
}
void Service::changedLocked() {
    dirty = true; ++registryRevision;
    if (checkpoint == Clock::time_point{}) checkpoint = Clock::now() + std::chrono::seconds(2);
    wake.notify_all();
}
void Service::recordLocked(const std::string& message) {
    if (diagnosticEvents.size() == 128) diagnosticEvents.pop_front();
    diagnosticEvents.push_back(message);
    if (diagnosticEvents.size() <= 24 || message.find("selected") != std::string::npos || message.find("failed") != std::string::npos)
        OutputDebugStringA(("Prappy analysis " + message + "\n").c_str());
}
void Service::requestLocked(const Identity& identity, analysis_queue::Priority priority, bool force) {
    if (!indexLoaded || readOnly) return;
    const auto key = idOf(identity);
    const auto it = registry.find(key);
    const bool ready = it != registry.end() && matching(it->second, identity) &&
        it->second.state == analysis_registry::State::Ready;
    if (ready && !force && !it->second.waveform.empty()) {
        if (priority == analysis_queue::Priority::Selected && !requestedWaveforms.count(key)) {
            readWaveform = identity; requestedWaveforms.insert(key); wake.notify_all();
        }
        return;
    }
    if (!force && it != registry.end() && matching(it->second, identity) &&
        it->second.state == analysis_registry::State::Failed && it->second.retryAfter > wallSeconds()) return;
    auto job = makeJob(identity, priority, force);
    queue.offer(job);
    if (priority == analysis_queue::Priority::Selected)
        recordLocked("enqueue selected " + job.key + " remote=" + (job.remote ? "yes" : "no") +
            " workers=" + std::to_string(workers.size()) + " remoteLimit=" +
            std::to_string(settings.remoteLimit));
    if (priority == analysis_queue::Priority::Selected)
        queue.preemptFor(job, workers.size(), settings.remoteLimit);
    wake.notify_all();
}
void Service::scheduleMissingLocked(bool force) {
    if (!indexLoaded || readOnly || catalog.empty()) return;
    if (force) for (const auto& [key, _] : catalog) forceKeys.insert(key);
    // Keep only a fixed window of background requests in memory. A selected
    // track bypasses this cap in requestLocked and can preempt a running job.
    constexpr size_t MaxWaiting = 256;
    auto it = backgroundCursor.empty() ? catalog.begin() : catalog.upper_bound(backgroundCursor);
    size_t inspected = 0;
    while (inspected++ < catalog.size() && queue.waiting() < MaxWaiting) {
        if (it == catalog.end()) it = catalog.begin();
        const auto& [key, identity] = *it;
        backgroundCursor = key;
        ++it;
        const bool requestedForce = forceKeys.contains(key);
        const auto stored = registry.find(key);
        if (!requestedForce && stored != registry.end() && matching(stored->second, identity) &&
            stored->second.state == analysis_registry::State::Ready) continue;
        const auto priority = selected && *selected == identity ? analysis_queue::Priority::Selected :
            analysis_queue::Priority::Background;
        if (queue.contains(makeJob(identity, priority, requestedForce))) continue;
        requestLocked(identity, priority, requestedForce);
    }
}
void Service::reconcile(const Library& library, bool force) {
    std::lock_guard lock(mutex);
    std::set<std::string> all;
    catalog.clear();
    // Catalog files include distinct canonical and duplicate-alias sources.
    // Share track presentation but not source-specific tags or decoder gain.
    // Both lists carry already scanned path/size/mtime metadata; no rehash.
    for (const auto& file : library.files) if (!file.offline) {
        Identity identity{file.path, file.bytes, file.stamp};
        auto key = idOf(identity); all.insert(key);
        catalog.insert_or_assign(std::move(key), std::move(identity));
    }
    for (const auto& track : library.tracks) if (!track.offline) {
        Identity identity{track.path, track.bytes, track.stamp};
        auto key = idOf(identity); all.insert(key);
        catalog.insert_or_assign(std::move(key), std::move(identity));
    }
    queue.retain(all);
    for (auto it = forceKeys.begin(); it != forceKeys.end();)
        if (!all.contains(*it)) it = forceKeys.erase(it); else ++it;
    if (force) forceKeys.insert(all.begin(), all.end());
    backgroundCursor.clear();
    snapshotDelivered = false; snapshotStarted = false;
    if (selected && !all.count(idOf(*selected))) selected.reset();
    if (force) requestedWaveforms.clear();
    if (!indexLoaded && force) forceAfterLoad = true;
    scheduleMissingLocked(force);
}
void Service::retry(const Track& track) {
    if (!track.offline) retry(Identity{track.path, track.bytes, track.stamp});
}
void Service::retry(const Identity& identity) {
    std::lock_guard lock(mutex);
    requestedWaveforms.erase(idOf(identity));
    requestLocked(identity, analysis_queue::Priority::Selected, true);
}
void Service::prioritize(const Track& track) {
    if (!track.offline) prioritize(Identity{track.path, track.bytes, track.stamp});
}
void Service::prioritize(const Identity& requested) {
    std::lock_guard lock(mutex);
    Identity identity = requested;
    if (selected != identity) requestedWaveforms.clear();
    selected = identity;
    queue.cancelOtherSelected(idOf(identity));
    requestLocked(identity, analysis_queue::Priority::Selected);
    recordLocked("selected request " + idOf(identity) + " pending=" + std::to_string(queue.waiting()));
}
void Service::lookahead(const std::vector<Identity>& tracks) {
    std::lock_guard lock(mutex);
    for (size_t i = 0; i < std::min<size_t>(3, tracks.size()); ++i) {
        if (selected && *selected == tracks[i]) continue;
        requestLocked(tracks[i], analysis_queue::Priority::Lookahead);
    }
}
std::optional<Summary> Service::snapshot(const Identity& identity) const {
    std::lock_guard lock(mutex);
    const auto it = registry.find(idOf(identity));
    if (it == registry.end() || !matching(it->second, identity) ||
        it->second.state != analysis_registry::State::Ready) return {};
    return summaryFor(it->second, identity);
}
std::optional<gain::Tags> Service::tagsFor(const Identity& identity) const {
    std::lock_guard lock(mutex);
    const auto it = registry.find(idOf(identity));
    if (it == registry.end() || !matching(it->second, identity) || !it->second.tagsProbed) return {};
    return it->second.tags;
}
std::vector<Summary> Service::initialSummaries(size_t limit) {
    std::vector<Summary> results;
    bool pending = false;
    {
        std::lock_guard lock(mutex);
        if (!indexLoaded || snapshotDelivered) return {};
        if (!snapshotStarted) { snapshotCursor = catalog.cbegin(); snapshotStarted = true; }
        // Bound both scanned identities and emitted summaries per UI tick.
        size_t scanned = 0;
        for (; snapshotCursor != catalog.cend() && scanned < 96 && results.size() < limit;
             ++snapshotCursor, ++scanned) {
            const auto& [key, identity] = *snapshotCursor;
            const auto it = registry.find(key);
            if (it != registry.end() && matching(it->second, identity) && it->second.state == analysis_registry::State::Ready)
                results.push_back(summaryFor(it->second, identity));
        }
        if (snapshotCursor == catalog.cend()) { snapshotDelivered = true; snapshotStarted = false; }
        else pending = true;
    }
    if (pending && notify) notify(); // Continue the bounded drain without busy waiting.
    return results;
}
std::vector<Summary> Service::collect(size_t limit) {
    std::lock_guard lock(mutex);
    std::vector<Summary> results;
    for (size_t i = 0; i < limit && !completed.empty(); ++i) {
        results.push_back(std::move(completed.front())); completed.pop_front();
    }
    return results;
}
void Service::completeLocked(Summary summary) {
    if (completed.size() >= 96) {
        requestedWaveforms.erase(idOf(completed.front().identity));
        completed.pop_front();
        recordLocked("completion queue capped");
    }
    completed.push_back(std::move(summary));
    // Keep waveform ownership bounded across in-flight completions, including
    // rapid selection changes. UI owns only its one displayed waveform.
    size_t bytes = 0;
    for (const auto& result : completed) if (result.waveform) bytes += waveformBytes(*result.waveform);
    while (bytes > maxWaveBytes) {
        const auto it = std::find_if(completed.begin(), completed.end(), [](const auto& item) { return item.waveform != nullptr; });
        if (it == completed.end()) break;
        bytes -= waveformBytes(*it->waveform);
        requestedWaveforms.erase(idOf(it->identity));
        it->waveform.reset();
    }
}
void Service::rememberTags(const Identity& identity, gain::Tags tags) {
    std::lock_guard lock(mutex);
    if (!indexLoaded || readOnly) return;
    auto& value = registry[idOf(identity)];
    if (!matching(value, identity)) {
        value = {}; value.path = idOf(identity); value.bytes = identity.bytes; value.stamp = identity.stamp;
    }
    value.tags = std::move(tags); value.tagsProbed = true;
    changedLocked();
}
std::string Service::status(const Identity& identity) const {
    std::lock_guard lock(mutex);
    if (!indexLoaded) return "Loading analysis registry";
    if (readOnly) return "Registry has an unsupported format (preserved)";
    const auto key = idOf(identity);
    const auto it = registry.find(key);
    const auto requested = makeJob(identity, analysis_queue::Priority::Background, false);
    if (queue.running(requested)) return "Analysis running";
    if (queue.contains(requested)) return "Analysis queued";
    if (it == registry.end() || !matching(it->second, identity)) return "Analysis not yet measured";
    if (it->second.state == analysis_registry::State::Ready) return "Analysis ready (cached summary)";
    if (it->second.state == analysis_registry::State::Offline) return "Offline (saved analysis retained)";
    if (it->second.state == analysis_registry::State::Failed) return "Analysis failed: " + it->second.error + " (retry on refresh)";
    return "Analysis pending";
}
std::vector<std::string> Service::diagnostics() const {
    std::lock_guard lock(mutex); return {diagnosticEvents.begin(), diagnosticEvents.end()};
}
void Service::decodeLoop(std::stop_token stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!stop.stop_requested()) {
        analysis_queue::Job job; std::stop_token taskToken;
        Identity identity;
        {
            std::unique_lock lock(mutex);
            if (!wake.wait(lock, stop, [&] { return queue.hasRunnable(settings.remoteLimit); })) break;
            auto ready = queue.take(settings.remoteLimit);
            if (!ready) continue;
            job = ready->first; taskToken = ready->second;
            // A forced source can exist before or after a catalog refresh.
            auto it = catalog.find(job.key);
            if (it != catalog.end() && it->second.bytes == job.bytes && it->second.stamp == job.stamp)
                identity = it->second;
            else {
                queue.complete(job, true, false); wake.notify_all(); continue;
            }
            recordLocked("start " + job.key + " priority=" + std::to_string(static_cast<unsigned>(job.priority)) +
                " queueWaitMs=" + std::to_string(elapsed(job.enqueuedAt)) +
                " active=" + std::to_string(queue.running()));
        }
        std::stop_callback stopTask(stop, [&] { // decoder receives its own per-job cancellation token
            std::lock_guard lock(mutex); queue.cancelAll(); wake.notify_all();
        });
        const auto begin = Clock::now();
        Summary summary; summary.identity = identity;
        bool cancelled = false;
        try {
            if (taskToken.stop_requested()) throw std::runtime_error("Analysis cancelled");
            if (identify(identity.path) != identity) throw std::runtime_error("Source changed before decode");
            const auto openMs = elapsed(begin);
            uint64_t firstPcmMs = 0;
            const auto pcm = [&](uint64_t) { if (!firstPcmMs) firstPcmMs = elapsed(begin); };
            gain::Tags observedTags;
            std::string observedCodec;
            bool tagsObserved = false;
            const auto metadata = [&](const auto& values, const std::string& codec) {
                observedTags = gain::parseTags(values, lower(codec).find("opus") != std::string::npos);
                observedCodec = codec;
                tagsObserved = true;
            };
            auto result = measure(identity.path, taskToken, pcm, metadata);
            if (taskToken.stop_requested()) throw std::runtime_error("Analysis cancelled");
            const auto decodedMs = elapsed(begin);
            if (identify(identity.path) != identity) throw std::runtime_error("Source changed during decode");
            summary.duration = result.duration(); summary.loudness = result.loudness;
            summary.samplePeak = peakOf(result);
            analysis_registry::Entry entry;
            entry.path = job.key; entry.bytes = identity.bytes; entry.stamp = identity.stamp;
            entry.frames = result.frames; entry.duration = summary.duration;
            entry.loudness = summary.loudness; entry.samplePeak = summary.samplePeak;
            entry.state = analysis_registry::State::Ready; entry.elapsedMs = elapsed(begin);
            entry.tags = std::move(observedTags); entry.tagsProbed = tagsObserved;
            entry.waveform = analysis_registry::blobName(entry);
            // Blob publication comes before the summary reference. A reader
            // never encounters a partially written artifact after a crash.
            fs::create_directories(directory);
            const auto serialized = serialize(identity, result);
            const auto serializationMs = elapsed(begin) - decodedMs;
            stateio::write(directory / entry.waveform, serialized, false);
            const auto publicationMs = elapsed(begin) - decodedMs - serializationMs;
            if (taskToken.stop_requested()) throw std::runtime_error("Analysis cancelled");
            if (waveformBytes(result) <= maxWaveBytes) {
                // selected is checked again while holding the mutex below
                summary.waveform = std::make_shared<Result>(std::move(result));
            }
            {
                std::lock_guard lock(mutex);
                const auto latest = catalog.find(job.key);
                if (taskToken.stop_requested() || latest == catalog.end() || latest->second != identity) {
                    cancelled = true;
                    recordLocked("discard stale/cancelled completion " + job.key);
                } else {
                    if (auto existing = registry.find(job.key); existing != registry.end() &&
                        matching(existing->second, identity) && existing->second.tagsProbed && !entry.tagsProbed) {
                        entry.tags = existing->second.tags; entry.tagsProbed = true;
                    }
                    registry.insert_or_assign(job.key, std::move(entry));
                    changedLocked();
                    if (!selected || *selected != identity) summary.waveform.reset();
                    if (selected && *selected == identity && summary.waveform) requestedWaveforms.insert(job.key);
                    recordLocked("complete " + job.key + " codec=" + observedCodec +
                        " duration=" + std::to_string(summary.duration) + " openCheckMs=" + std::to_string(openMs) +
                        " firstPcmMs=" + std::to_string(firstPcmMs) + " decodeTotalMs=" + std::to_string(decodedMs) +
                        " serializationMs=" + std::to_string(serializationMs) +
                        " artifactWriteMs=" + std::to_string(publicationMs) +
                        " publishTotalMs=" + std::to_string(elapsed(begin)));
                    completeLocked(std::move(summary));
                }
            }
        } catch (const std::exception& e) {
            cancelled = stop.stop_requested() || taskToken.stop_requested();
            if (!cancelled) {
                summary.error = e.what();
                std::lock_guard lock(mutex);
                // Only persist durable errors if the identity remains current.
                const auto found = catalog.find(job.key);
                if (found != catalog.end() && found->second == identity) {
                    auto& entry = registry[job.key];
                    if (!matching(entry, identity)) {
                        entry = {}; entry.path = job.key; entry.bytes = identity.bytes; entry.stamp = identity.stamp;
                    }
                    // A corrupt/missing blob does not invalidate an existing
                    // good loudness measurement.
                    if (entry.state != analysis_registry::State::Ready) {
                        entry.state = analysis_registry::State::Failed;
                        entry.error = summary.error.substr(0, 512);
                        entry.retryAfter = wallSeconds() + 60;
                        entry.elapsedMs = elapsed(begin);
                        changedLocked();
                    }
                    completeLocked(std::move(summary));
                }
                recordLocked("failed " + job.key + " " + e.what());
            }
        }
        {
            std::lock_guard lock(mutex);
            queue.complete(job, cancelled, !stop.stop_requested());
            if (!cancelled) forceKeys.erase(job.key);
            if (!stop.stop_requested()) scheduleMissingLocked(false);
        }
        wake.notify_all();
        if (notify) notify();
    }
}
void Service::ioLoop(std::stop_token stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    analysis_registry::Entries recovered;
    bool unsupported = false;
    bool recoveredBackup = false;
    const auto index = directory / "summaries.bin";
    const auto backup = fs::path(index.wstring() + L".bak");
    std::error_code queryError;
    const bool hasIndex = fs::exists(index, queryError);
    if (queryError) unsupported = true; // Never clobber an inaccessible index.
    if (hasIndex && !unsupported) {
        try { recovered = analysis_registry::decode(analysis_registry::readFile(index)); }
        catch (const analysis_registry::Unsupported&) { unsupported = true; }
        catch (const std::exception&) {
            try { recovered = analysis_registry::decode(analysis_registry::readFile(backup)); recoveredBackup = true; }
            catch (const analysis_registry::Unsupported&) { unsupported = true; }
            catch (const std::exception&) { recovered.clear(); }
        }
    } else if (!unsupported && fs::exists(backup, queryError)) {
        // Interrupted replacement can leave a backup without the primary.
        try { recovered = analysis_registry::decode(analysis_registry::readFile(backup)); recoveredBackup = true; }
        catch (const analysis_registry::Unsupported&) { unsupported = true; }
        catch (const std::exception&) { recovered.clear(); }
    }
    if (queryError) unsupported = true;
    {
        std::lock_guard lock(mutex);
        registry = std::move(recovered); readOnly = unsupported; indexLoaded = true;
        if (recoveredBackup && !readOnly) changedLocked();
        scheduleMissingLocked(forceAfterLoad); forceAfterLoad = false;
        if (selected) requestLocked(*selected, analysis_queue::Priority::Selected);
        recordLocked("registry load entries=" + std::to_string(registry.size()) + (readOnly ? " unsupported" : ""));
    }
    if (notify) notify();
    wake.notify_all();
    for (;;) {
        std::optional<Identity> toRead;
        analysis_registry::Entries checkpointEntries;
        uint64_t version = 0;
        bool saving = false;
        {
            std::unique_lock lock(mutex);
            const auto due = [&] { return dirty && !readOnly && Clock::now() >= checkpoint; };
            if (!stop.stop_requested() && !readWaveform && !due()) {
                if (dirty && !readOnly) wake.wait_until(lock, checkpoint, [&] { return stop.stop_requested() || readWaveform.has_value(); });
                else wake.wait(lock, [&] { return stop.stop_requested() || readWaveform.has_value() || (dirty && !readOnly); });
            }
            if (readWaveform && !stop.stop_requested()) {
                toRead = *readWaveform; readWaveform.reset();
            } else if (dirty && !readOnly && (stop.stop_requested() || due())) {
                checkpointEntries = registry; version = registryRevision; saving = true;
            } else if (stop.stop_requested()) break;
        }
        if (toRead) {
            const auto id = *toRead;
            analysis_registry::Entry entry;
            {
                std::lock_guard lock(mutex);
                auto it = registry.find(idOf(id));
                if (it == registry.end() || !matching(it->second, id) || it->second.waveform.empty()) continue;
                entry = it->second;
            }
            const auto begin = Clock::now();
            try {
                auto result = parse(analysis_registry::readFile(directory / entry.waveform), id);
                if (waveformBytes(result) > maxWaveBytes) throw std::runtime_error("Waveform exceeds memory budget");
                std::lock_guard lock(mutex);
                if (selected && *selected == id) {
                    auto summary = summaryFor(entry, id); summary.waveform = std::make_shared<Result>(std::move(result));
                    completeLocked(std::move(summary));
                    recordLocked("selected waveform cache hit " + idOf(id) + " readMs=" + std::to_string(elapsed(begin)));
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex);
                if (selected && *selected == id) {
                    // An oversized blob is not corrupt: re-decoding cannot
                    // solve the configured memory constraint.
                    if (std::string_view(e.what()) != "Waveform exceeds memory budget") {
                        auto job = makeJob(id, analysis_queue::Priority::Selected, true);
                        queue.offer(job); queue.preemptFor(job, workers.size(), settings.remoteLimit);
                        wake.notify_all();
                    }
                    recordLocked("selected waveform cache miss " + idOf(id) + " " + e.what());
                }
            }
            if (notify) notify();
        }
        if (saving) {
            try {
                analysis_registry::atomicWrite(index, analysis_registry::encode(checkpointEntries));
                std::lock_guard lock(mutex);
                if (registryRevision == version) { dirty = false; checkpoint = {}; }
                else checkpoint = Clock::now() + std::chrono::seconds(2);
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex);
                recordLocked(std::string("registry checkpoint failed: ") + e.what());
                checkpoint = Clock::now() + std::chrono::seconds(5);
                if (stop.stop_requested()) break;
            }
        }
    }
}
}
