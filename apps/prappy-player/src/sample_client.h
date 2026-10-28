#pragma once
#include "paths.h"
#include <json.hpp>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace sampling {
using Json = nlohmann::json;
struct Connection { std::string url = "http://127.0.0.1:8765", token; fs::path credentialFile; bool startWorkstation=false; };
struct Request {
    std::string tag, method = "GET", path;
    Json body;
    uint64_t generation = 0;
    fs::path download;
};
struct Reply {
    Request request;
    Json body;
    std::vector<float> pcm;
    std::string error;
    int status = 0;
};
Reply perform(const Connection&, const Request&, std::stop_token = {});
std::string uuid();
std::string fileHash(const fs::path&);
class Client {
    Connection connection;
    std::function<void()> notify;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Request> pending;
    std::vector<Reply> completed;
    std::jthread worker;
public:
    Client(Connection, std::function<void()>);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void submit(Request);
    std::vector<Reply> collect();
};
}
