#pragma once
#include "paths.h"
#include "state_io.h"
#include <json.hpp>

namespace stateio {
inline nlohmann::json evaluationState(const fs::path& root,bool required=false){
    const auto path=root/"evaluation-state.json";
    if(!fs::exists(path)){
        if(required)throw std::runtime_error("Evaluation requires its isolated state marker.");
        return nullptr;
    }
    auto marker=nlohmann::json::parse(read(path));
    if(marker.at("mode")!="indexed-evaluation"||marker.at("protocol")!=3||marker.at("url")!="http://127.0.0.1:18816")
        throw std::runtime_error("Invalid indexed evaluation scope.");
    const auto declared=pathFromText(marker.at("state_root").get<std::string>());
    const auto original=pathFromText(marker.at("source_state_root").get<std::string>());
    if(declared.empty()||original.empty())throw std::runtime_error("Evaluation state roots are required.");
    const auto actual=key(fs::weakly_canonical(root)),expected=key(fs::weakly_canonical(declared)),stable=key(fs::weakly_canonical(original));
    const auto within=[](const std::wstring& value,std::wstring parent){
        if(value==parent)return true;
        if(!parent.ends_with(L"\\"))parent+=L'\\';
        return value.starts_with(parent);
    };
    if(actual!=expected||within(actual,stable)||within(stable,actual))
        throw std::runtime_error("Evaluation state must be separate from stable state.");
    return marker;
}
inline void evaluationConnection(const fs::path& root,const std::string& url,const fs::path& token){
    const auto marker=evaluationState(root);
    if(!marker.is_null()&&(url!=marker.at("url").get<std::string>()||key(token)!=key(pathFromText(marker.at("token_file").get<std::string>()))))
        throw std::runtime_error("Evaluation must use its isolated backend and credentials.");
}
}
