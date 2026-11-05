#pragma once
#include "library.h"
#include "sample_client.h"

bool refreshPreparedCatalog(Library&,const fs::path& config,std::stop_token,ScanProgress&);
