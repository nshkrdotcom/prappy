#pragma once
#include "library.h"
#include "sample_client.h"

bool refreshPreparedCatalog(Library&,const fs::path& config,std::stop_token,ScanProgress&);
bool restorePreparedCatalog(Library&,const fs::path& config,std::stop_token,ScanProgress&);
sampling::Connection preparedConnection(const fs::path& stateDirectory);
std::optional<prepared::CatalogSource> readResumeSource(const fs::path&);
std::string serializeResumeSource(const prepared::CatalogSource&);
struct Audio;
std::function<bool(const fs::path&,bool,double)> preparedPlaybackLoader(Audio&,Library&,std::function<void()>);
std::optional<fs::path> preparedOriginalLocation(const Track&,const Library&);
