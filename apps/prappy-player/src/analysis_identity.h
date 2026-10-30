#pragma once
#include "analysis.h"
#include "library.h"
namespace analysis {
// Saved metadata may locate a local display preview while a remote root is
// being checked. Gain selection always uses the default online-only lookup.
inline std::optional<Identity> catalogIdentity(const Library& library,const fs::path& path,bool savedPreview=false){
    const auto id=key(path);
    const auto track=std::find_if(library.tracks.begin(),library.tracks.end(),[&](const Track& item){return Library::matches(item,id);});
    if(track==library.tracks.end()||(track->offline&&!savedPreview))return {};
    if(key(track->path)==id)return Identity{track->path,track->bytes,track->stamp};
    const auto alias=std::find_if(library.files.begin(),library.files.end(),[&](const Track& item){return item.id==id;});
    if(alias==library.files.end()||(alias->offline&&!savedPreview))return {};
    return Identity{alias->path,alias->bytes,alias->stamp};
}
}
