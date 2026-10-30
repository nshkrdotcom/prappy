#pragma once
// Portable scheduler state machine. Callers hold one mutex; decoder threads
// never hold it during filesystem, pipe, cache or DSP operations.
#include <cstdint>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>
namespace analysis_queue {
enum class Priority : unsigned { Background = 1, Lookahead = 2, Selected = 3 };
struct Job {
    std::string key;
    uint64_t bytes = 0;
    int64_t stamp = 0;
    Priority priority = Priority::Background;
    bool remote = false;
    bool force = false;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point enqueuedAt = std::chrono::steady_clock::now();
    bool sameSource(const Job& other) const {
        return key == other.key && bytes == other.bytes && stamp == other.stamp;
    }
};
struct Running {
    Job job;
    std::stop_source stop;
    bool cancelledForPriority = false;
};
class Queue {
    std::map<std::string, Job> waiting_;
    std::map<std::string, Running> running_;
    uint64_t sequence_ = 0;
public:
    size_t waiting() const { return waiting_.size(); }
    size_t running() const { return running_.size(); }
    size_t remoteRunning() const;
    bool contains(const Job& job) const;
    bool running(const Job& job) const;
    void offer(Job job);
    bool preemptFor(const Job& priority, size_t workers, size_t remoteLimit);
    bool hasRunnable(size_t remoteLimit) const;
    std::optional<std::pair<Job, std::stop_token>> take(size_t remoteLimit);
    void complete(const Job& job, bool cancelled, bool retry);
    void retain(const std::set<std::string>& keys);
    void cancelOtherSelected(const std::string& key);
    void cancelAll();
};
size_t automaticWorkers(unsigned hardwareCount);
}
