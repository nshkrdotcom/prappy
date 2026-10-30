#pragma once
#include "analysis.h"
#include "analysis_queue.h"
#include "analysis_registry.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_set>
struct Library;
struct Track;
namespace analysis {
struct ServiceSettings {
    unsigned workers = 0;      // 0 = hardware_count - 2, clamped to [1,4]
    unsigned remoteLimit = 2;  // [1,4]
    unsigned waveformMemoryMiB = 128; // [16,512]
};
class Service {
    using Clock = std::chrono::steady_clock;
    fs::path directory;
    ServiceSettings settings;
    std::function<void()> notify;
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    analysis_queue::Queue queue;
    std::map<std::string, Identity> catalog;
    std::map<std::string, Identity>::const_iterator snapshotCursor;
    bool snapshotStarted = false;
    analysis_registry::Entries registry;
    std::deque<Summary> completed;
    std::optional<Identity> selected;
    std::optional<Identity> readWaveform;
    std::unordered_set<std::string> requestedWaveforms;
    std::set<std::string> forceKeys; // forced rescan remains eligible across batches
    std::string backgroundCursor;
    std::deque<std::string> diagnosticEvents;
    std::vector<std::jthread> workers;
    std::jthread ioThread;
    bool indexLoaded = false, snapshotDelivered = false, readOnly = false, dirty = false, forceAfterLoad = false;
    uint64_t registryRevision = 0;
    Clock::time_point checkpoint{};
    size_t maxWaveBytes = 0;
    void decodeLoop(std::stop_token stop);
    void ioLoop(std::stop_token stop);
    void scheduleMissingLocked(bool force);
    void requestLocked(const Identity& id, analysis_queue::Priority priority, bool force = false);
    void completeLocked(Summary summary);
    void changedLocked();
    void recordLocked(const std::string& message);
    static bool matching(const analysis_registry::Entry& entry, const Identity& identity);
    static Summary summaryFor(const analysis_registry::Entry& entry, const Identity& identity);
public:
    Service(fs::path cacheDirectory, ServiceSettings limits, std::function<void()> wakeUI = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    void reconcile(const Library& library, bool force = false);
    void prioritize(const Track& track);
    void prioritize(const Identity& identity);
    void retry(const Track& track);
    void retry(const Identity& identity);
    void lookahead(const std::vector<Identity>& tracks);
    std::optional<Summary> snapshot(const Identity& identity) const;
    std::optional<gain::Tags> tagsFor(const Identity& identity) const;
    std::vector<Summary> initialSummaries(size_t limit = 48);
    std::vector<Summary> collect(size_t limit = 48);
    void rememberTags(const Identity& identity, gain::Tags tags);
    std::string status(const Identity& identity) const;
    std::vector<std::string> diagnostics() const;
    size_t workerCount() const { return workers.size(); }
};
}
