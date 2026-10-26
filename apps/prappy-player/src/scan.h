#pragma once
#include "catalog.h"
// One cancellable scan, started only on launch or by the Refresh button.
// Worker owns its result; the UI swaps it in only after completion.
struct Scan {
    ScanProgress progress;
    std::stop_source cancel;
    std::future<std::pair<Library, bool>> job;
    std::jthread worker;
    std::function<void()> notify;
    ~Scan() { cancel.request_stop(); if (job.valid()) job.wait(); }
    bool running() const { return job.valid(); }
    void begin(const fs::path& config, int order, const Library* previous = nullptr, bool persist = false) {
        if (running()) return;
        progress.found = progress.checked = progress.total = 0; progress.dedup = false;
        progress.hashReads = progress.hashReused = 0;
        cancel = std::stop_source{};
        Library baseline; if (previous) baseline = *previous;
        std::promise<std::pair<Library, bool>> promise; job = promise.get_future();
        worker = std::jthread([this, config, order, persist, next = std::move(baseline), stop = cancel.get_token(), notify = notify, promise = std::move(promise)] () mutable {
            next.order = order;
            bool success = next.refresh(config, stop, &progress);
            if (success && persist && !stop.stop_requested()) {
                try { saveCatalog(config.parent_path() / "player-catalog.bin", next); next.catalogBackup = true; }
                catch (const std::exception& e) { next.notice += " | Catalog save failed: " + std::string(e.what()); }
            }
            promise.set_value(std::make_pair(std::move(next), success));
            if (notify) notify();
        });
    }
    bool update(Library& library) {
        if (!running() || job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        auto [next, success] = job.get();
        if (success) { next.order = library.order; next.sort(); library = std::move(next); }
        else library.notice = std::move(next.notice);
        return success;
    }
    std::string status() const {
        if (progress.dedup) return "Checking duplicates: " + std::to_string(progress.checked.load()) + " / " + std::to_string(progress.total.load());
        return "Scanning music: " + std::to_string(progress.found.load()) + " files";
    }
};
