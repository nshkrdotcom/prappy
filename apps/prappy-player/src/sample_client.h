#pragma once
#include "paths.h"
#include "prepared_audio.h"
#include "native_session.h"
#include <json.hpp>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <array>
#include <memory>
#include <chrono>
namespace sampling {
using Json = nlohmann::json;
struct Connection { std::string url = "http://127.0.0.1:8765", token; fs::path credentialFile; bool startWorkstation=false; };
Connection authorize(Connection,std::stop_token = {});
struct Request {
    std::string tag, method = "GET", path;
    Json body;
    uint64_t generation = 0;
    fs::path download;
    std::chrono::steady_clock::time_point queuedAt{};
    uint64_t diagnosticId=0;
    PcmBudget::Permit pcmMemory,incomingMemory;
};
struct Reply {
    Request request;
    Json body;
    std::vector<float> pcm;
    std::shared_ptr<const prepared::Block> preparedPcm;
    std::string error;
    int status = 0;
};
Reply perform(const Connection&, const Request&, std::stop_token = {});
std::string uuid();
enum class ReadScope { Audition, PCM, Audio };
struct ClientState;
class Client {
public:
    using Transport=std::function<Reply(const Connection&,const Request&,std::stop_token)>;
    using CredentialReader=std::function<std::string(const fs::path&,std::stop_token)>;
    using Bootstrap=std::function<void()>;
private:
    std::shared_ptr<ClientState> state;
public:
    Client(Connection, std::function<void()>,Transport = {},CredentialReader = {},Bootstrap = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void submit(Request);
    void cancelReads(ReadScope);
    std::vector<Reply> collect();
};
}
