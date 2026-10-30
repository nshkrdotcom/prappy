#include "analysis_queue.h"
#include <algorithm>
#include <limits>
namespace analysis_queue {
size_t automaticWorkers(unsigned hardwareCount) {
    return std::clamp(static_cast<size_t>(hardwareCount > 2 ? hardwareCount - 2 : 1), size_t(1), size_t(4));
}
size_t Queue::remoteRunning() const {
    return std::count_if(running_.begin(), running_.end(), [](const auto& pair) { return pair.second.job.remote; });
}
bool Queue::contains(const Job& job) const {
    if (auto it = waiting_.find(job.key); it != waiting_.end() && it->second.sameSource(job)) return true;
    if (auto it = running_.find(job.key); it != running_.end() && it->second.job.sameSource(job)) return true;
    return false;
}
bool Queue::running(const Job& job) const {
    if (auto it = running_.find(job.key); it != running_.end()) return it->second.job.sameSource(job);
    return false;
}
void Queue::offer(Job job) {
    if (auto running = running_.find(job.key); running != running_.end()) {
        if (running->second.job.sameSource(job) && !(job.force && !running->second.job.force)) {
            // A running decode already produces both waveform and summary.
            if (static_cast<unsigned>(job.priority) > static_cast<unsigned>(running->second.job.priority))
                running->second.job.priority = job.priority;
            return;
        }
        running->second.stop.request_stop();
    }
    auto it = waiting_.find(job.key);
    if (it != waiting_.end() && it->second.sameSource(job)) {
        it->second.priority = std::max(it->second.priority, job.priority);
        it->second.force = it->second.force || job.force;
        return;
    }
    job.sequence = ++sequence_;
    waiting_.insert_or_assign(job.key, std::move(job));
}
bool Queue::preemptFor(const Job& demand, size_t workers, size_t remoteLimit) {
    const bool busy = running_.size() >= workers;
    const bool remoteBusy = demand.remote && remoteRunning() >= remoteLimit;
    if (!busy && !remoteBusy) return false;
    Running* victim = nullptr;
    for (auto& [_, task] : running_) {
        if (task.stop.stop_requested() || (remoteBusy && !task.job.remote)) continue;
        if (task.job.priority >= demand.priority) continue;
        if (!victim || task.job.priority < victim->job.priority ||
            (task.job.priority == victim->job.priority && task.job.sequence < victim->job.sequence)) victim = &task;
    }
    if (!victim) return false;
    victim->cancelledForPriority = true;
    victim->stop.request_stop();
    return true;
}
bool Queue::hasRunnable(size_t remoteLimit) const {
    for (const auto& [key, job] : waiting_)
        if (!running_.count(key) && (!job.remote || remoteRunning() < remoteLimit)) return true;
    return false;
}
std::optional<std::pair<Job, std::stop_token>> Queue::take(size_t remoteLimit) {
    auto best = waiting_.end();
    for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
        if (running_.count(it->first) || (it->second.remote && remoteRunning() >= remoteLimit)) continue;
        if (best == waiting_.end() || it->second.priority > best->second.priority ||
            (it->second.priority == best->second.priority && it->second.sequence < best->second.sequence)) best = it;
    }
    if (best == waiting_.end()) return {};
    auto job = best->second;
    waiting_.erase(best);
    auto [it, inserted] = running_.emplace(job.key, Running{job, std::stop_source{}, false});
    (void)inserted;
    return std::pair{job, it->second.stop.get_token()};
}
void Queue::complete(const Job& job, bool cancelled, bool retry) {
    auto it = running_.find(job.key);
    if (it == running_.end() || !it->second.job.sameSource(job)) return;
    const bool interrupted = it->second.cancelledForPriority;
    const auto latest = it->second.job;
    running_.erase(it);
    if (cancelled && retry && interrupted && !waiting_.count(job.key)) {
        auto resume = latest; resume.priority = Priority::Background;
        offer(resume);
    }
}
void Queue::retain(const std::set<std::string>& keys) {
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (!keys.count(it->first)) it = waiting_.erase(it); else ++it;
    }
    for (auto& [key, running] : running_) if (!keys.count(key)) running.stop.request_stop();
}
void Queue::cancelOtherSelected(const std::string& key) {
    for (auto& [id, running] : running_)
        if (id != key && running.job.priority == Priority::Selected) {
            running.cancelledForPriority = true; running.stop.request_stop();
        }
    for (auto& [id, waiting] : waiting_)
        if (id != key && waiting.priority == Priority::Selected) waiting.priority = Priority::Background;
}
void Queue::cancelAll() { for (auto& [_, value] : running_) value.stop.request_stop(); waiting_.clear(); }
}
