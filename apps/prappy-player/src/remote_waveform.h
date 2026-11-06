#pragma once
#include "prepared_catalog.h"
#include "sample_client.h"
#include "analysis_dsp.h"

class RemoteWaveform {
    std::function<void()> notify;
    std::unique_ptr<sampling::Client> client;
    sampling::Connection connection;
    std::string identity,message;
    prepared::CatalogSource source;
    uint64_t generation=0;
    std::chrono::steady_clock::time_point pollAt{};
    std::shared_ptr<const analysis::Result> waveform;
public:
    explicit RemoteWaveform(std::function<void()> wake):notify(std::move(wake)){}
    void select(const prepared::CatalogSource&,const sampling::Connection&,bool force=false);
    bool collect();
    void clear(){if(client)client->cancelReads(sampling::ReadScope::Audio);++generation;identity.clear();waveform.reset();message.clear();}
    const auto& value()const{return waveform;}
    const std::string& status()const{return message;}
};
