#pragma once
#include "sample_client.h"
#include <future>
#include <ostream>

inline void sampleClientCancellationTests(std::ostream& log) {
    using namespace sampling;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    struct ReleaseOnExit {std::promise<void>& promise;~ReleaseOnExit(){try{promise.set_value();}catch(const std::future_error&){}}};
    // The release gate is deliberately independent of cancellation. Destruction
    // must return while a filesystem/provider call has not acknowledged stop.
    for(const bool credentialRead:{false,true}) {
        std::promise<void> entered,release,cleaned,cancelled;
        ReleaseOnExit releaseOnExit{release};
        auto gate=release.get_future().share();
        struct Lifetime {std::promise<void>* done;~Lifetime(){done->set_value();}};
        auto lifetime=std::make_shared<Lifetime>();lifetime->done=&cleaned;
        Connection connection;connection.token=std::string(32,'x');
        Client::Transport transport=[&,gate,lifetime](const Connection&,const Request& request,std::stop_token stop){
            if(!credentialRead){std::stop_callback cancellation(stop,[&]{cancelled.set_value();});entered.set_value();gate.wait();}
            Reply result;result.request=request;return result;
        };
        Client::CredentialReader reader;
        if(credentialRead){connection.token.clear();reader=[&,gate,lifetime](const fs::path&,std::stop_token stop){std::stop_callback cancellation(stop,[&]{cancelled.set_value();});entered.set_value();gate.wait();return std::string(32,'x');};}
        std::atomic<int> notifications=0;
        auto client=std::make_unique<Client>(connection,[&]{++notifications;},std::move(transport),std::move(reader));
        lifetime.reset();
        Request request;request.tag="libraries";request.path="/libraries";request.generation=19;client->submit(request);
        const bool started=entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        if(!started){release.set_value();require(false,"Sampling operation did not reach read barrier.");}
        auto destroyed=std::async(std::launch::async,[&]{client.reset();});
        const bool returned=destroyed.wait_for(std::chrono::seconds(2))==std::future_status::ready;
        release.set_value();destroyed.get();
        require(returned,"Sampling Client destruction waited for blocked read/transport.");
        require(cancelled.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Sampling retirement did not request transport/read cancellation.");
        require(cleaned.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Retired sampling operation retained its state after release.");
        require(notifications==0,"Retired sampling operation notified a destroyed owner.");
    }
    {
        std::promise<void> overviewEntered,metadataEntered,release,played;
        ReleaseOnExit releaseOnExit{release};
        auto gate=release.get_future().share();
        Connection connection;connection.token=std::string(32,'x');
        Client::Transport blocked=[&,gate](const Connection&,const Request& request,std::stop_token){
            if(request.tag=="overview"){overviewEntered.set_value();gate.wait();}
            if(request.tag=="hash"){metadataEntered.set_value();gate.wait();}
            Reply result;result.request=request;return result;
        };
        auto previous=std::make_unique<Client>(connection,[] {},std::move(blocked));
        Request overview;overview.tag="overview";previous->submit(overview);
        Request hash;hash.tag="hash";previous->submit(hash);
        const bool overviewBlocked=overviewEntered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        const bool metadataBlocked=metadataEntered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        if(!overviewBlocked||!metadataBlocked){release.set_value();require(false,"Sampling lanes did not reach barriers.");}
        previous.reset();
        Client replacement(connection,[&]{played.set_value();},[](const Connection& c,const Request&,std::stop_token){
            if(c.token!=std::string(32,'x'))throw std::runtime_error("Sampling request lost its immutable credentials.");
            // Client must retain the submitted request even with a custom transport.
            Reply result;return result;
        });
        Request pcm;pcm.tag="pcm";pcm.path="foreground";pcm.generation=71;replacement.submit(pcm);
        const bool foregroundCompleted=played.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        release.set_value();
        require(foregroundCompleted,"Foreground PCM waited behind retired overview/source work.");
        const auto replies=replacement.collect();
        require(replies.size()==1&&replies[0].request.generation==71&&replies[0].request.path=="foreground"&&replies[0].error.empty(),"Sampling reply lost request generation or connection identity.");
    }
    {
        std::promise<void> entered,release,completed;
        ReleaseOnExit releaseOnExit{release};
        auto gate=release.get_future().share();std::atomic<int> reads=0,finished=0;
        Connection connection;
        Client client(connection,[&]{if(++finished==2)completed.set_value();},[](const Connection&,const Request&,std::stop_token){return Reply{};},
            [&,gate](const fs::path&,std::stop_token){++reads;entered.set_value();gate.wait();return std::string(32,'x');});
        Request pcm;pcm.tag="pcm";client.submit(pcm);Request browse;browse.tag="browse";client.submit(browse);
        const bool reading=entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        if(!reading){release.set_value();require(false,"Credential read did not start.");}
        require(client.collect().empty(),"HTTP work began before startup credentials were ready.");
        release.set_value();
        require(completed.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Credential preparation did not release foreground/background lanes.");
        require(reads==1&&client.collect().size()==2,"Connection credentials were read more than once.");
    }
    {
        std::promise<void> entered,release;
        ReleaseOnExit releaseOnExit{release};auto gate=release.get_future().share();
        Connection connection;connection.token=std::string(32,'x');
        Client client(connection,[] {},[&,gate](const Connection&,const Request& request,std::stop_token){
            if(request.tag=="hash"){entered.set_value();gate.wait();}return Reply{};
        });
        Request request;request.tag="hash";client.submit(request);
        require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Queue test did not reach source barrier.");
        request.method="POST";request.tag="queued";
        for(int index=0;index<512;++index)client.submit(request);
        bool rejected=false;try{client.submit(request);}catch(const std::runtime_error&){rejected=true;}
        require(rejected,"Sampling pending request queue exceeded its 512-request bound.");
    }
    {
        std::promise<void> completed;
        std::atomic<int> reads=0,bootstraps=0,requests=0;
        Connection connection;connection.startWorkstation=true;
        Client client(connection,[&]{if(++requests==2)completed.set_value();},[](const Connection& prepared,const Request&,std::stop_token){
            if(prepared.token!=std::string(32,'c'))throw std::runtime_error("Cold startup sent HTTP without prepared credentials.");
            return Reply{};
        },[&](const fs::path&,std::stop_token){
            ++reads;if(!bootstraps)throw std::runtime_error("Credential not created yet.");return std::string(32,'c');
        },[&]{++bootstraps;});
        Request pcm;pcm.tag="pcm";client.submit(pcm);Request browse;browse.tag="browse";client.submit(browse);
        require(completed.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Cold workstation credential creation never released pending HTTP work.");
        const auto replies=client.collect();
        require(reads==2&&bootstraps==1&&replies.size()==2&&replies[0].error.empty()&&replies[1].error.empty(),"Cold bootstrap did not cache its first successful credential read.");
    }
    log<<"PASS sampling retirement returns before blocked credential/transport release and cleans up without notification\n";
    log<<"PASS foreground PCM bypasses retired browse/source work and credentials initialize once per connection\n";
    log<<"PASS sampling pending requests remain bounded at 512\n";
    log<<"PASS cold workstation bootstrap creates credentials before foreground/background HTTP work\n";
}
