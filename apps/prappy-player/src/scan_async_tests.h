#pragma once
#include "scan.h"
#include <ostream>

inline void scanAsyncTests(std::ostream& log) {
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    std::promise<void> entered,release,cleaned;
    auto gate=release.get_future().share();
    struct ReleaseOnExit {std::promise<void>& promise;~ReleaseOnExit(){try{promise.set_value();}catch(const std::future_error&){}}} releaseOnExit{release};
    struct Lifetime {std::promise<void>* done;~Lifetime(){done->set_value();}};
    auto lifetime=std::make_shared<Lifetime>();lifetime->done=&cleaned;
    std::atomic<int> notifications=0;
    auto scan=std::make_unique<Scan>([&,gate,lifetime](Library&,const fs::path&,std::stop_token,ScanProgress& progress){
        progress.found=27;entered.set_value();gate.wait();return true;
    });
    lifetime.reset();scan->notify=[&]{++notifications;};scan->begin({},0);
    require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Scan did not reach filesystem barrier.");
    require(scan->progress.found==27,"Scan progress did not retain its public atomic access.");
    auto destroyed=std::async(std::launch::async,[&]{scan.reset();});
    const bool returned=destroyed.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    release.set_value();destroyed.get();
    require(returned,"Scan destruction waited for blocked filesystem work.");
    require(cleaned.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Retired scan retained state after provider release.");
    require(notifications==0,"Retired scan notified its destroyed owner.");
    log<<"PASS scan retirement returns before blocked filesystem release and cleans up without notification\n";
    {
        std::promise<void> blocked,releaseActive,stopSeen,finished;
        auto activeGate=releaseActive.get_future().share();ReleaseOnExit releaseActiveOnExit{releaseActive};
        Scan active([&,activeGate](Library&,const fs::path&,std::stop_token stop,ScanProgress&){
            std::stop_callback stopped(stop,[&]{stopSeen.set_value();});blocked.set_value();activeGate.wait();return true;
        });
        active.begin({},0);
        require(blocked.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Active scan did not reach coalescing barrier.");
        active.cancel.request_stop();
        require(stopSeen.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Public scan cancellation did not reach active work.");
        std::atomic<int> supersededRuns=0;
        std::vector<std::unique_ptr<Scan>> superseded;
        for(int index=0;index<32;++index) {
            auto pending=std::make_unique<Scan>([&](Library&,const fs::path&,std::stop_token,ScanProgress&){++supersededRuns;return true;});
            pending->begin({},0);superseded.push_back(std::move(pending));
        }
        Library baseline;baseline.verified.emplace(L"cached-source",Verification{43,19,{},{}});
        Scan latest([](Library& next,const fs::path&,std::stop_token,ScanProgress&){
            if(next.order!=2||!next.verified.contains(L"cached-source"))throw std::runtime_error("Scan dropped its baseline order or source verification cache.");
            Track a,b;a.id=L"a";a.sortName="a";a.duration=30.;b.id=L"b";b.sortName="b";b.duration=10.;next.tracks={a,b};return true;
        });
        latest.notify=[&]{finished.set_value();};latest.begin({},2,&baseline);
        require(supersededRuns==0,"Pending scan ran while filesystem provider held the only worker.");
        Library unchanged;Track retained;retained.id=L"retained";unchanged.tracks.push_back(retained);
        for(auto& old:superseded) {
            require(old->job.wait_for(std::chrono::seconds(0))==std::future_status::ready,"Superseded pending scan retained an unbounded outstanding job.");
            require(!old->update(unchanged)&&unchanged.tracks[0].id==L"retained","Superseded scan replaced the current library.");
        }
        Library library;library.order=3;releaseActive.set_value();
        require(finished.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Latest pending scan did not run after provider release.");
        require(latest.update(library)&&!latest.running()&&library.order==3&&library.tracks[0].id==L"b"&&library.verified.contains(L"cached-source"),"Successful scan failed to preserve current sort order or verification cache.");
        require(active.job.wait_for(std::chrono::seconds(0))==std::future_status::ready&&!active.update(unchanged)&&unchanged.tracks[0].id==L"retained","Cancelled active scan replaced the current library.");
        require(supersededRuns==0,"Coalesced scan executed stale pending work.");
    }
    log<<"PASS scan pending work coalesces, public cancellation preserves library, and successful scans retain current order/cache\n";
}
