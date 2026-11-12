#pragma once
#include "paths.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
namespace debuglog {
using Observer=void(*)(const std::string&,const std::string&) noexcept;
inline std::atomic<Observer> observer=nullptr;
inline void setObserver(Observer value)noexcept{observer=value;}
using Clock=std::chrono::steady_clock;
inline long long elapsed(Clock::time_point start){return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();}
class Log {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> pending;
    fs::path path;
    size_t limit=1024*1024, dropped=0;
    bool stopping=false,writing=false;
    std::atomic<bool> enabled=false;
    Clock::time_point start=Clock::now();
    std::thread worker;
    void run() {
        for(;;) {
            std::deque<std::string> batch;fs::path destination;size_t budget=0,lost=0;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock,[&]{return stopping||!pending.empty();});
                if(pending.empty()&&stopping)return;
                batch.swap(pending);destination=path;budget=limit;lost=std::exchange(dropped,0);writing=true;
            }
            // All disk work stays on this thread; diagnostics never fail playback.
            try {
                if(lost)batch.push_front("log dropped="+std::to_string(lost)+"\n");
                std::error_code ec;auto size=fs::file_size(destination,ec);if(ec)size=0;
                std::ofstream out(destination,std::ios::app|std::ios::binary);
                for(const auto& row:batch) {
                    if(size+row.size()>budget) {
                        out.close();const auto backup=fs::path(destination.wstring()+L".1");
                        fs::remove(backup,ec);ec.clear();fs::rename(destination,backup,ec);
                        if(ec)break;
                        out.open(destination,std::ios::trunc|std::ios::binary);size=0;
                    }
                    out.write(row.data(),static_cast<std::streamsize>(row.size()));size+=row.size();
                }
            }catch(...){}
            {std::lock_guard lock(mutex);writing=false;}changed.notify_all();
        }
    }
public:
    Log():worker([this]{run();}){}
    ~Log(){enabled=false;{std::lock_guard lock(mutex);stopping=true;}changed.notify_all();worker.join();}
    Log(const Log&)=delete;
    Log& operator=(const Log&)=delete;
    void configure(const fs::path& destination,bool active,size_t budget=1024*1024) {
        enabled=false;flush();
        {std::lock_guard lock(mutex);path=destination;limit=std::max<size_t>(1024,budget);}
        enabled=active;
    }
    void write(const std::string& event,const std::string& detail) noexcept {
        if(const auto callback=observer.load(std::memory_order_relaxed))callback(event,detail);
        if(!enabled.load(std::memory_order_relaxed))return;
        try {
            SYSTEMTIME time{};GetSystemTime(&time);char stamp[96]{};
            std::snprintf(stamp,sizeof(stamp),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ +%lld pid=%lu ",
                time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,elapsed(start),GetCurrentProcessId());
            auto row=std::string(stamp)+event+" "+detail;
            if(row.size()>766)row.resize(766);
            for(auto& c:row)if(static_cast<unsigned char>(c)<32||c==127)c=' ';
            row+='\n';
            {std::lock_guard lock(mutex);if(!enabled)return;if(pending.size()==512){++dropped;return;}pending.push_back(std::move(row));}
            changed.notify_one();
        }catch(...){}
    }
    void flush(){std::unique_lock lock(mutex);changed.wait(lock,[&]{return pending.empty()&&!writing;});}
};
inline Log& instance(){static Log log;return log;}
inline void write(const std::string& event,const std::string& detail){instance().write(event,detail);}
}
