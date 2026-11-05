#pragma once
#include "library.h"
#include "sample_client.h"
#include "analysis_dsp.h"

class RemoteWaveform {
    std::function<void()> notify;
    std::unique_ptr<sampling::Client> client;
    sampling::Connection connection;
    std::string identity,message;
    prepared::CatalogSource source;
    uint64_t generation=0;
    std::shared_ptr<const analysis::Result> waveform;
public:
    explicit RemoteWaveform(std::function<void()> wake):notify(std::move(wake)){}
    void select(const Track&,const sampling::Connection&,bool force=false);
    bool collect();
    void clear(){++generation;identity.clear();waveform.reset();message.clear();}
    const auto& value()const{return waveform;}
    const std::string& status()const{return message;}
};
