#pragma once
#include <atomic>
// Only the playback actor calls libmpv after initialization. Its mutex protects
// small value snapshots and queued intents, and is never held across a core call.
struct Audio::Async {
    struct Work { int kind; uint64_t epoch, revision; std::function<void(Audio&)> action; };
    std::mutex mutex, notificationMutex;
    std::condition_variable changed;
    bool stopping=false, busy=false, eof=false;
    uint64_t issued=0, completed=0, snapshotRevision=0, eofSerial=0;
    std::deque<Work> queue;
    std::unique_ptr<Audio> core, snapshot;
    std::thread thread;
    std::atomic<bool> dirty{false};
    std::function<void()> notify;
    static void copy(Audio& to,const Audio& from) {
        to.error=from.error;to.current=from.current;to.loaded=from.loaded;to.active=from.active;
        to.paused=from.paused;to.pauseRequested=from.pauseRequested;to.waitingStart=from.waitingStart;to.sequentialRetry=from.sequentialRetry;
        to.desiredEntry=from.desiredEntry;to.eventEntry=from.eventEntry;to.serial=from.serial;to.loadedSerial=from.loadedSerial;
        to.position=from.position;to.duration=from.duration;to.positionTime=from.positionTime;
        to.info=from.info;to.gain=from.gain;to.volume=from.volume;to.meterPeak=from.meterPeak;to.meterAvailable=from.meterAvailable;
        to.cachedTags=from.cachedTags;to.seekPreview=from.seekPreview;to.queuedSeek=from.queuedSeek;to.pendingSeek=from.pendingSeek;
    }
};
// Retired playback cores remain owned here until their blocking operation
// returns. The process-static reaper joins after runPlayer has released its
// window and singleton. No retired actor keeps a pointer into the UI Audio.
class AudioWorkerReaper {
    static constexpr size_t Capacity=8;
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::shared_ptr<Audio::Async>,Capacity> retired;
    size_t actors=0,head=0,count=0;
    bool stopping=false;
    std::thread thread;
    void run(){
        for(;;){
            std::shared_ptr<Audio::Async> state;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||count;});
                if(!count)return;
                state=std::move(retired[head]);head=(head+1)%Capacity;--count;
            }
            if(state->thread.joinable())state->thread.join();
            // Callback captures state fields: remove it and destroy mpv before
            // releasing the final state owner.
            state->core.reset();state.reset();
            {std::lock_guard lock(mutex);--actors;}
        }
    }
public:
    AudioWorkerReaper():thread([this]{run();}){}
    ~AudioWorkerReaper(){ {std::lock_guard lock(mutex);stopping=true;}changed.notify_all();thread.join(); }
    static AudioWorkerReaper& instance(){static AudioWorkerReaper reaper;return reaper;}
    void check(){std::lock_guard lock(mutex);if(actors>=Capacity)throw std::runtime_error("Playback is still closing; try opening again shortly.");}
    void reserve(){std::lock_guard lock(mutex);if(actors>=Capacity)throw std::runtime_error("Playback is still closing; try opening again shortly.");++actors;}
    void retire(std::shared_ptr<Audio::Async> state){
        {std::lock_guard lock(mutex);retired[(head+count)%Capacity]=std::move(state);++count;}
        changed.notify_one();
    }
};
inline void Audio::checkWorkerCapacity(){AudioWorkerReaper::instance().check();}
inline void Audio::startWorker() {
    if(worker)return;
    auto state=std::make_shared<Async>();state->core=std::make_unique<Audio>();state->snapshot=std::make_unique<Audio>();
    AudioWorkerReaper::instance().reserve();worker=state;
    auto& core=*state->core;core.handle=handle;core.dll=dll;dll=nullptr;
#define MPV_COPY(name) core.name=name
    MPV_COPY(create);MPV_COPY(initialize);MPV_COPY(terminate_destroy);MPV_COPY(set_option_string);
    MPV_COPY(command);MPV_COPY(set_property);MPV_COPY(get_property);MPV_COPY(wait_event);MPV_COPY(error_string);MPV_COPY(free);
    MPV_COPY(set_wakeup_callback);MPV_COPY(command_async);MPV_COPY(observe_property);MPV_COPY(free_node_contents);
#undef MPV_COPY
    Async::copy(core,*this);core.outputMeter=std::move(outputMeter);
    core.setNotify([state=state.get()]{
        {std::lock_guard lock(state->mutex);state->dirty=true;}
        state->changed.notify_one();
    });
    state->thread=std::thread([state=state.get()] {
        uint64_t tagsEpoch=0;
        for(;;) {
            std::optional<Async::Work> work;
            {std::unique_lock lock(state->mutex);
                const auto ready=[&]{return state->stopping||!state->queue.empty()||state->dirty.load();};
                if(state->core->loaded && state->core->active && !state->core->paused)
                    state->changed.wait_for(lock,std::chrono::milliseconds(10),ready);
                else state->changed.wait(lock,ready);
                if(state->stopping)break;
                state->busy=true;
                if(!state->queue.empty()){work=std::move(state->queue.front());state->queue.pop_front();}
            }
            const bool dirty=state->dirty.exchange(false);
            auto& player=*state->core;
            std::function<void()> notify;
            try {
                if(work && (work->kind==0 || work->epoch==player.serial))work->action(player);
                const bool eof=player.poll();
                if(player.loaded && tagsEpoch!=player.loadedSerial){player.cachedTags=player.loadedTags();tagsEpoch=player.loadedSerial;}
                auto snapshot=std::make_unique<Audio>();Async::copy(*snapshot,player);
                {std::lock_guard lock(state->mutex);
                    if(work)state->completed=work->revision;
                    state->snapshotRevision=state->completed;
                    state->snapshot=std::move(snapshot);if(eof){state->eof=true;state->eofSerial=player.serial;}state->busy=false;
                    if(dirty||work)notify=state->notify;
                }
            }catch(const std::exception& e){
                std::lock_guard lock(state->mutex);state->snapshot->error=e.what();state->snapshot->serial=player.serial;
                if(work)state->completed=work->revision;state->snapshotRevision=state->completed;state->busy=false;
            }
            state->changed.notify_all();
            if(notify){
                std::lock_guard gate(state->notificationMutex);
                {std::lock_guard lock(state->mutex);if(state->stopping)notify={};}
                if(notify)notify();
            }
        }
    });
}
inline void Audio::stopWorker(){
    if(!worker)return;
    auto state=std::move(worker);
    {
        std::lock_guard gate(state->notificationMutex);
        std::lock_guard lock(state->mutex);state->stopping=true;state->queue.clear();state->notify={};
    }
    state->changed.notify_all();
    // The actor owns destruction and the loaded DLL; public handle is borrowed.
    handle=nullptr;AudioWorkerReaper::instance().retire(std::move(state));
}
inline void Audio::enqueue(int kind,std::function<void(Audio&)> action,bool replace){
    auto& state=*worker;
    {std::lock_guard lock(state.mutex);
        if(replace)state.queue.clear();
        else std::erase_if(state.queue,[&](const Async::Work& work){return work.kind==kind;});
        state.queue.push_back({kind,serial,++state.issued,std::move(action)});
    }
    state.changed.notify_one();
}
inline bool Audio::collectWorker(){
    auto& state=*worker;bool eof=false;
    {std::lock_guard lock(state.mutex);
        if(state.snapshot && state.snapshot->serial==serial && state.snapshotRevision==state.issued){
            const auto queued=queuedSeek,preview=seekPreview;
            Async::copy(*this,*state.snapshot);queuedSeek=queued;
            if(preview && (paused || queuedSeek || std::abs(position-*preview)>=.25))seekPreview=preview;
            eof=state.eof && state.eofSerial==serial;state.eof=false;
        }
    }
    if(loaded)submitSeek();
    // The native output meter also lives on the actor. No core reads occur here.
    return eof;
}
inline void Audio::waitIdle(){
    if(!worker)return;auto& state=*worker;
    std::unique_lock lock(state.mutex);state.changed.wait(lock,[&]{return !state.busy&&state.queue.empty()&&state.completed==state.issued;});
    lock.unlock();collectWorker();
}

inline void Audio::setWorkerNotify(){std::lock_guard lock(worker->mutex);worker->notify=notify;}
