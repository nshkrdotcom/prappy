#pragma once
#include "paths.h"
#include <optional>
#include <string_view>

namespace sampling {
inline bool localSourceImportServer(std::string_view url){
    const auto scheme=url.find("://");
    if(scheme==url.npos||(url.substr(0,scheme)!="http"&&url.substr(0,scheme)!="https"))return false;
    url.remove_prefix(scheme+3);
    const auto authority=url.substr(0,url.find_first_of("/?#"));
    const auto hostEnd=authority.starts_with('[')?authority.find(']')+1:authority.find(':');
    const auto host=authority.substr(0,hostEnd);
    if(host!="127.0.0.1"&&host!="localhost"&&host!="[::1]")return false;
    const auto tail=hostEnd==authority.npos?std::string_view{}:authority.substr(hostEnd);
    return tail.empty()||(tail.starts_with(':')&&tail.size()>1&&tail.substr(1).find_first_not_of("0123456789")==tail.npos);
}
inline std::optional<std::string> sourceImportPath(fs::path path,std::string_view server){
    if(!localSourceImportServer(server))return {};
    path=displayPath(path);path.make_preferred();
    const auto id=key(path);
    if(id.starts_with(L"\\\\wsl.localhost\\ubuntu26-dev\\")||id.starts_with(L"\\\\wsl$\\ubuntu26-dev\\"))
        return wslLinuxPath(path);
    const auto native=path.wstring();
    if(native.size()>3&&native[1]==L':'&&native[2]==L'\\'&&
       ((native[0]>=L'A'&&native[0]<=L'Z')||(native[0]>=L'a'&&native[0]<=L'z'))){
        auto tail=native.substr(3);std::replace(tail.begin(),tail.end(),L'\\',L'/');
        const auto drive=static_cast<char>(native[0]>=L'a'?native[0]:native[0]+(L'a'-L'A'));
        return "/mnt/"+std::string(1,drive)+"/"+utf8(fs::path(tail));
    }
    return {};
}
}
