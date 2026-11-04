#pragma once
#include "debug_log.h"
#include <fstream>
#include <sstream>
inline void debugLogTests(const fs::path& root,std::ostream& output) {
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto read=[](const fs::path& path){std::ifstream in(path);return std::string(std::istreambuf_iterator<char>(in),{});};
    debuglog::Log log;
    const auto path=root/"debug.log";
    fs::remove(path);fs::remove(path.string()+".1");
    log.configure(path,true,1024);
    log.write("open","serial=7 source=example\nforged\trow");log.flush();
    auto text=read(path);
    require(text.find("serial=7")!=std::string::npos,"Debug log lost playback identity.");
    require(std::count(text.begin(),text.end(),'\n')==1,"Debug log allowed forged multiline records.");
    log.configure(path,false,1024);log.write("disabled","must not appear");log.flush();
    require(read(path)==text,"Disabled debug logging wrote a record.");
    log.configure(path,true,1024);
    for(int i=0;i<20;++i){log.write("rotation",std::string(600,'x'));log.flush();}
    require(fs::exists(path.string()+".1")&&fs::file_size(path)<=1024&&fs::file_size(path.string()+".1")<=1024,"Debug logs exceeded rotation budget.");
    log.configure(root/"unwritable"/"missing"/"debug.log",true);
    log.write("failure","Logging failure must not stop playback.");log.flush();
    output<<"PASS debug log: identity, one-line records, disabled flag, bounded rotation and unavailable destination\n";
}
