#pragma once
#include "sample_client.h"
#include <future>
#include <ostream>

inline void sampleClientCancellationTests(std::ostream& log) {
    using namespace sampling;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    struct ReleaseOnExit {std::promise<void>& promise;~ReleaseOnExit(){try{promise.set_value();}catch(const std::future_error&){}}};
    {
        std::promise<void> entered,release,completed;
        ReleaseOnExit releaseOnExit{release};auto gate=release.get_future().share();
        std::atomic<int> replies=0;
        Client client(Connection{},[&]{if(++replies==2)completed.set_value();},
            [](const Connection&,const Request&,std::stop_token){return Reply{};},
            [&,gate](const fs::path&,std::stop_token){entered.set_value();gate.wait();return std::string(32,'x');});
        require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Prepared coalescing barrier did not start.");
        client.submit({"source-pcm","GET","native-old",{{"domain","native"},{"audition",false}},1,{}});
        client.submit({"source-pcm","GET","monitor",{{"domain","native"},{"audition",true}},1,{}});
        client.submit({"source-pcm","GET","native-latest",{{"domain","native"},{"audition",false}},1,{}});
        release.set_value();
        require(completed.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Region window replacement cancelled pending audition audio.");
        const auto result=client.collect();
        require(result.size()==2&&result[0].request.path=="monitor"&&result[1].request.path=="native-latest","Native request coalescing retained an obsolete region window.");
    }
    {
        std::promise<void> entered,latest;std::mutex mutex;std::condition_variable_any wake;
        Connection connection;connection.token=std::string(32,'x');
        Client client(connection,[]{},[&](const Connection&,const Request& request,std::stop_token stop){
            if(request.path=="old-window"){
                entered.set_value();std::unique_lock lock(mutex);wake.wait(lock,stop,[]{return false;});
            }else latest.set_value();
            Reply result;result.request=request;return result;
        });
        client.submit({"source-pcm","GET","old-window",{{"audition",false}},1,{}});
        require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Old source read did not begin.");
        client.submit({"source-waveform","GET","latest-view",{{"overview",false}},2,{}});
        require(latest.get_future().wait_for(std::chrono::milliseconds(100))==std::future_status::ready,"New source waited for an obsolete in-flight decode.");
    }
    {
        std::promise<void> entered,release,closed;ReleaseOnExit releaseOnExit{release};auto gate=release.get_future().share();
        Connection connection;connection.token=std::string(32,'x');std::atomic<int> notifications=0;
        Client blocker(connection,[]{},[&](const Connection&,const Request&,std::stop_token){entered.set_value();gate.wait();return Reply{};});
        blocker.submit({"command","POST","lane-barrier",nullptr,0,{}});
        require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Session cleanup barrier did not start.");
        {
            Client owner(connection,[&]{++notifications;},[&](const Connection& c,const Request& request,std::stop_token stop){
                if(request.tag=="source-session-close"&&request.method=="DELETE"&&c.token==connection.token&&!stop.stop_requested())closed.set_value();return Reply{};
            });
            owner.submit({"source-session-close","DELETE","/source/session?owner=retiring",nullptr,0,{}});
        }
        release.set_value();
        require(closed.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready,"Transport destruction discarded its decoder-session close.");
        require(notifications==0,"Session cleanup notified a destroyed owner.");
    }
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
            if(request.tag=="browse"){overviewEntered.set_value();gate.wait();}
            if(request.tag=="command"){metadataEntered.set_value();gate.wait();}
            Reply result;result.request=request;return result;
        };
        auto previous=std::make_unique<Client>(connection,[] {},std::move(blocked));
        Request overview;overview.tag="browse";previous->submit(overview);
        Request hash;hash.tag="command";hash.method="POST";previous->submit(hash);
        const bool overviewBlocked=overviewEntered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        const bool metadataBlocked=metadataEntered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
        if(!overviewBlocked||!metadataBlocked){release.set_value();require(false,"Sampling lanes did not reach barriers.");}
        previous.reset();
        Client replacement(connection,[&]{played.set_value();},[](const Connection& c,const Request&,std::stop_token){
            if(c.token!=std::string(32,'x'))throw std::runtime_error("Sampling request lost its immutable credentials.");
            // Client must retain the submitted request even with a custom transport.
            Reply result;return result;
        });
        Request pcm;pcm.tag="source-pcm";pcm.path="foreground";pcm.generation=71;replacement.submit(pcm);
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
        Request pcm;pcm.tag="source-pcm";client.submit(pcm);Request browse;browse.tag="browse";client.submit(browse);
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
            if(request.tag=="command"){entered.set_value();gate.wait();}return Reply{};
        });
        Request request;request.tag="command";request.method="POST";client.submit(request);
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
        Request pcm;pcm.tag="source-pcm";client.submit(pcm);Request browse;browse.tag="browse";client.submit(browse);
        require(completed.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Cold workstation credential creation never released pending HTTP work.");
        const auto replies=client.collect();
        require(reads==2&&bootstraps==1&&replies.size()==2&&replies[0].error.empty()&&replies[1].error.empty(),"Cold bootstrap did not cache its first successful credential read.");
    }
    log<<"PASS sampling retirement returns before blocked credential/transport release and cleans up without notification\n";
    log<<"PASS foreground PCM bypasses retired browse/source work and credentials initialize once per connection\n";
    log<<"PASS sampling pending requests remain bounded at 512\n";
    log<<"PASS cold workstation bootstrap creates credentials before foreground/background HTTP work\n";
}
