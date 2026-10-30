#pragma once
#include "audio.h"
#include <future>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace audio_async_tests {
struct Backend {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false, destroyed = false;
    void (*wakeup)(void*)=nullptr;void* wakeContext=nullptr;
    size_t wakeNotifications=0,destroys=0;
    std::vector<std::string> calls;
    int64_t entry = 0;
    std::deque<std::pair<mpv_event_id,int64_t>> events;
    mpv_event currentEvent{};mpv_event_start_file start{};
    char tagKey[23]="REPLAYGAIN_TRACK_GAIN",tagValue[6]="-6 dB";
    char* tagKeys[1]={tagKey};mpv_node tagNode{};mpv_node_list tagList{};
    Backend(){tagNode.format=MPV_FORMAT_STRING;tagNode.u.string=tagValue;tagList.num=1;tagList.keys=tagKeys;tagList.values=&tagNode;}
    static Backend* instance;
    static int set(mpv_handle*, const char* name, mpv_format format, void* data) {
        auto& self = *instance;
        std::unique_lock lock(self.mutex);
        if (!self.entered) { self.entered = true; self.changed.notify_all(); self.changed.wait(lock,[&]{return self.released;}); }
        self.calls.push_back(std::string(name)+"="+(format==MPV_FORMAT_FLAG?std::to_string(*static_cast<int*>(data)):std::to_string(*static_cast<double*>(data))));
        return 0;
    }
    static int command(mpv_handle*, const char** args) { auto& s=*instance;std::lock_guard lock(s.mutex); s.calls.push_back(args[0]+std::string(":")+(args[1]?args[1]:""));if(std::string(args[0])=="loadfile"){++s.entry;s.events.push_back({MPV_EVENT_START_FILE,s.entry});s.events.push_back({MPV_EVENT_FILE_LOADED,s.entry});}return 0; }
    static int get(mpv_handle*,const char* name,mpv_format,void* data) { if(std::string(name)=="playlist/0/id"){*static_cast<int64_t*>(data)=instance->entry;return 0;}if(std::string(name)=="metadata"){auto& node=*static_cast<mpv_node*>(data);node.format=MPV_FORMAT_NODE_MAP;node.u.list=&instance->tagList;return 0;}return MPV_ERROR_PROPERTY_UNAVAILABLE; }
    static mpv_event* event(mpv_handle*,double) { auto& s=*instance;s.currentEvent={};if(!s.events.empty()){auto [id,entry]=s.events.front();s.events.pop_front();s.currentEvent.event_id=id;s.start.playlist_entry_id=entry;s.currentEvent.data=&s.start;}return &s.currentEvent; }
    static void wake(mpv_handle*,void(*callback)(void*),void* context) { instance->wakeup=callback;instance->wakeContext=context; }
    static void freeNode(mpv_node*) {}
    static void destroy(mpv_handle*) {auto& self=*instance;std::lock_guard lock(self.mutex);self.destroyed=true;++self.destroys;self.changed.notify_all();}
    static const char* error(int) { return "fake error"; }
};
inline Backend* Backend::instance = nullptr;
inline void run(std::ostream& log) {
    Backend backend; Backend::instance=&backend;
    Audio audio; audio.handle=reinterpret_cast<mpv_handle*>(1);audio.set_property=Backend::set;audio.command=Backend::command;audio.get_property=Backend::get;audio.wait_event=Backend::event;audio.set_wakeup_callback=Backend::wake;audio.terminate_destroy=Backend::destroy;audio.error_string=Backend::error;audio.free_node_contents=Backend::freeNode;
    audio.startWorker();
    auto request=std::async(std::launch::async,[&]{audio.play("blocked.wav",true);audio.setPaused(true);audio.setVolume(23);audio.setGain(-6);audio.poll();return true;});
    bool returned=request.wait_for(std::chrono::milliseconds(200))==std::future_status::ready;
    {std::unique_lock lock(backend.mutex);if(!backend.changed.wait_for(lock,std::chrono::seconds(2),[&]{return backend.entered;}))throw std::runtime_error("Fake source did not enter blocking core call.");}
    // Flooding replacements must stay bounded, and only the latest queued
    // source may commit after the already executing source finishes.
    for(int i=0;i<500;++i)audio.play("superseded.wav",true);
    audio.play("latest.wav",true);audio.setGain(-9);audio.setPaused(false);audio.poll();
    {std::lock_guard lock(backend.mutex);backend.released=true;} backend.changed.notify_all();request.get();
    if(!returned)throw std::runtime_error("Playback UI methods waited on blocked libmpv source/control.");
    audio.waitIdle();
    if(audio.current!="latest.wav" || !audio.loaded || audio.loadedSerial!=audio.serial)throw std::runtime_error("Stale blocked playback committed over latest intent.");
    if(audio.loadedTags()!=std::vector<std::pair<std::string,std::string>>{{"REPLAYGAIN_TRACK_GAIN","-6 dB"}})throw std::runtime_error("Playback readiness omitted gain metadata snapshot.");
    {std::lock_guard lock(backend.mutex);
        bool latest=false,gain=false,unpaused=false;
        for(const auto& call:backend.calls){
            if(call.find("superseded.wav")!=std::string::npos)throw std::runtime_error("Superseded playback intent reached libmpv.");
            if(call.find("latest.wav")!=std::string::npos)latest=true;
            if(latest && call=="volume-gain=-9.000000")gain=true;
            if(latest && call=="pause=0"){if(!gain)throw std::runtime_error("Playback unpaused before latest gain applied.");unpaused=true;}
        }
        if(!gain||!unpaused)throw std::runtime_error("Latest gain/play intent was lost.");
    }
    audio.setPaused(true);audio.waitIdle();
    audio.setNotify([&]{std::lock_guard lock(backend.mutex);++backend.wakeNotifications;backend.changed.notify_all();});
    // A paused actor sleeps until explicitly woken by libmpv. This handshake
    // verifies that the event wake is delivered without a periodic UI poll.
    backend.wakeup(backend.wakeContext);
    {std::unique_lock lock(backend.mutex);if(!backend.changed.wait_for(lock,std::chrono::seconds(2),[&]{return backend.wakeNotifications>0;}))throw std::runtime_error("Idle playback actor lost explicit core wake.");}
    audio.setNotify({});
    audio.stopWorker();
    {std::unique_lock lock(backend.mutex);if(!backend.changed.wait_for(lock,std::chrono::seconds(2),[&]{return backend.destroyed;}))throw std::runtime_error("Idle playback actor cleanup did not complete.");backend.entered=backend.released=backend.destroyed=false;}
    auto closing=std::make_unique<Audio>();closing->handle=reinterpret_cast<mpv_handle*>(1);closing->set_property=Backend::set;closing->command=Backend::command;closing->get_property=Backend::get;closing->wait_event=Backend::event;closing->set_wakeup_callback=Backend::wake;closing->terminate_destroy=Backend::destroy;closing->error_string=Backend::error;closing->free_node_contents=Backend::freeNode;
    closing->startWorker();closing->setNotify([&]{std::lock_guard lock(backend.mutex);++backend.wakeNotifications;});
    const auto notificationsBeforeClose=backend.wakeNotifications;
    closing->play("blocked-close.wav",true);
    {std::unique_lock lock(backend.mutex);if(!backend.changed.wait_for(lock,std::chrono::seconds(2),[&]{return backend.entered;}))throw std::runtime_error("Closing playback actor missed barrier.");}
    auto destroy=std::async(std::launch::async,[&]{closing.reset();});
    const bool closed=destroy.wait_for(std::chrono::milliseconds(200))==std::future_status::ready;
    bool prematurelyDestroyed=false;
    {std::lock_guard lock(backend.mutex);prematurelyDestroyed=backend.destroyed;backend.released=true;}backend.changed.notify_all();destroy.get();
    {std::unique_lock lock(backend.mutex);if(!backend.changed.wait_for(lock,std::chrono::seconds(2),[&]{return backend.destroyed;}))throw std::runtime_error("Retired playback actor was not reclaimed after core release.");}
    if(!closed)throw std::runtime_error("Closing Audio waited for blocked libmpv.");
    if(prematurelyDestroyed)throw std::runtime_error("Retired playback core was destroyed while still executing.");
    if(backend.wakeNotifications!=notificationsBeforeClose)throw std::runtime_error("Retired playback actor notified destroyed UI.");
    std::vector<std::unique_ptr<Audio>> capacity;
    bool rejected=false;
    for(size_t i=0;i<9;++i){
        try{
            Audio::checkWorkerCapacity();
            auto item=std::make_unique<Audio>();item->handle=reinterpret_cast<mpv_handle*>(1);item->set_property=Backend::set;item->command=Backend::command;item->get_property=Backend::get;item->wait_event=Backend::event;item->set_wakeup_callback=Backend::wake;item->terminate_destroy=Backend::destroy;item->error_string=Backend::error;item->free_node_contents=Backend::freeNode;
            item->startWorker();capacity.push_back(std::move(item));
        }catch(const std::runtime_error&){rejected=true;break;}
    }
    if(!rejected||capacity.size()>8)throw std::runtime_error("Playback actor retirement capacity was not bounded.");
    size_t cleaned=0;{std::lock_guard lock(backend.mutex);cleaned=backend.destroys+capacity.size();}
    capacity.clear();
    {std::unique_lock lock(backend.mutex);if(!backend.changed.wait_for(lock,std::chrono::seconds(2),[&]{return backend.destroys>=cleaned;}))throw std::runtime_error("Bounded retired playback actors were not reclaimed.");}
    log << "PASS blocked libmpv playback commands remain responsive; bounded latest-source replacement, gain-before-unpause, wake and retirement lifecycle\n";
}
}
