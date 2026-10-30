#pragma once
#include "catalog.h"
#include <condition_variable>
namespace scan_detail {
using Action=std::function<bool(Library&,const fs::path&,std::stop_token,ScanProgress&)>;
struct State {
    ScanProgress progress;
    std::mutex notificationMutex;
    std::function<void()> notify;
    bool retired=false;
};
struct Task {
    std::shared_ptr<State> state;
    Action action;
    fs::path config;
    Library next;
    std::stop_source cancel;
    std::promise<std::pair<Library,bool>> promise;
    bool persist=false;
    void complete(bool success) {
        promise.set_value(std::make_pair(std::move(next),success));
        std::lock_guard guard(state->notificationMutex);
        if(!state->retired&&state->notify)state->notify();
    }
};
// One active scan and one replaceable pending scan across UI lifetimes. Shared
// task state outlives a retired Scan; the worker joins only at process teardown.
class Executor {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::shared_ptr<Task> active,pending;
    std::jthread worker;
    void run(std::stop_token shutdown) {
        while(!shutdown.stop_requested()) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock guard(mutex);
                if(!wake.wait(guard,shutdown,[&]{return static_cast<bool>(pending);})||shutdown.stop_requested())return;
                task=std::move(pending);active=task;
            }
            const auto stop=task->cancel.get_token();
            bool success=false;
            {
                // Destroy this callback before advancing to the next task, so
                // a late stop can never cancel another scan's filesystem read.
                std::stop_callback cancellation(stop,[this]{CancelSynchronousIo(worker.native_handle());});
                try {
                    if(!stop.stop_requested())success=task->action?task->action(task->next,task->config,stop,task->state->progress):task->next.refresh(task->config,stop,&task->state->progress);
                    if(stop.stop_requested())success=false;
                    if(success&&task->persist) {
                        try{saveCatalog(task->config.parent_path()/"player-catalog.bin",task->next);task->next.catalogBackup=true;}
                        catch(const std::exception& error){task->next.notice+=" | Catalog save failed: "+std::string(error.what());}
                    }
                    if(stop.stop_requested())success=false;
                }catch(const std::exception& error){task->next.notice=error.what();}
            }
            task->complete(success);
            {std::lock_guard guard(mutex);active.reset();}
        }
    }
    Executor():worker([this](std::stop_token stop){run(stop);}){}
public:
    static Executor& instance(){static Executor executor;return executor;}
    ~Executor() {
        worker.request_stop();std::shared_ptr<Task> current;
        {std::lock_guard guard(mutex);current=active;pending.reset();}
        if(current)current->cancel.request_stop();
        wake.notify_all();if(worker.joinable())worker.join();
    }
    void submit(std::shared_ptr<Task> task) {
        std::shared_ptr<Task> replaced;
        {std::lock_guard guard(mutex);replaced=std::move(pending);pending=std::move(task);}
        if(replaced){replaced->cancel.request_stop();replaced->next.notice="Scan superseded.";replaced->complete(false);}
        wake.notify_all();
    }
    void retire(const std::shared_ptr<State>& state) {
        std::lock_guard guard(mutex);
        if(pending&&pending->state==state)pending.reset();
    }
};
}
// One cancellable scan, started only on launch or by the Refresh button.
// Worker owns its result; the UI swaps it in only after completion.
struct Scan {
private:
    std::shared_ptr<scan_detail::State> state=std::make_shared<scan_detail::State>();
public:
    using Action=scan_detail::Action;
    Action action;
    ScanProgress& progress;
    explicit Scan(Action scanAction = {}):action(std::move(scanAction)),progress(state->progress){}
    std::stop_source cancel;
    std::future<std::pair<Library, bool>> job;
    std::function<void()> notify;
    ~Scan() {
        {std::lock_guard guard(state->notificationMutex);state->retired=true;state->notify={};}
        cancel.request_stop();if(job.valid())scan_detail::Executor::instance().retire(state);
    }
    bool running() const { return job.valid(); }
    void begin(const fs::path& config, int order, const Library* previous = nullptr, bool persist = false) {
        if (running()) return;
        progress.found = progress.checked = progress.total = 0; progress.dedup = false;
        progress.hashReads = progress.hashReused = 0;
        cancel = std::stop_source{};
        Library baseline; if (previous) baseline = *previous;
        auto task=std::make_shared<scan_detail::Task>();task->state=state;task->action=action;task->config=config;
        task->next=std::move(baseline);task->next.order=order;task->cancel=cancel;task->persist=persist;
        {std::lock_guard guard(state->notificationMutex);state->notify=notify;}
        job=task->promise.get_future();scan_detail::Executor::instance().submit(std::move(task));
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
