#pragma once
#include "sample_library.h"
#include "state_io.h"
#include <bit>
#include <cstring>
#include <condition_variable>
#include <mutex>

namespace sampling {
// A disposable, previously verified canonical preview. The immutable source
// identity and checksum are checked before any samples reach the editor.
struct Preview {
    static constexpr Frame MaxFrames=Rate*30;
    static constexpr size_t MaxBytes=static_cast<size_t>(MaxFrames)*8+512*1024+256;
    SourceRef source;
    std::string blob,decoder;
    Frame total=0,start=0;
    LibraryJson overview;
    std::vector<float> pcm;
    bool matches(const SourceRef& ref,const std::string& hash,Frame frames)const{
        return source==ref&&blob==hash&&total==frames;
    }
    void validate()const{
        source.validate();
        if(blob.size()!=64||blob.find_first_not_of("0123456789abcdef")!=std::string::npos||
           total<=0||start<0||start>total||pcm.size()%2||pcm.size()/2>static_cast<size_t>(MaxFrames)||
           static_cast<Frame>(pcm.size()/2)>total-start||decoder.size()>1024||(!pcm.empty()&&decoder.empty()))
            throw std::runtime_error("Invalid saved canonical preview.");
        for(const auto value:pcm)if(!std::isfinite(value))throw std::runtime_error("Nonfinite saved preview.");
        if(!overview.is_null()){
            const Frame step=overview.at("frames_per_bin"),origin=overview.value("origin_frame",Frame{0});
            const auto& peaks=overview.at("peaks");
            if(step<=0||step>total||origin<0||origin>=total||!peaks.is_array()||peaks.size()>8192||
               peaks.size()>static_cast<uint64_t>((total-origin)/step+((total-origin)%step!=0)))
                throw std::runtime_error("Invalid saved preview overview.");
            for(const auto& p:peaks){
                if(!p.is_array()||p.size()!=4)throw std::runtime_error("Invalid saved preview peak.");
                for(const auto& value:p)if(!value.is_number()||!std::isfinite(value.get<double>()))throw std::runtime_error("Nonfinite saved preview peak.");
                if(p[0].get<double>()>p[1].get<double>()||p[2].get<double>()>p[3].get<double>())throw std::runtime_error("Invalid saved preview peak bounds.");
            }
        }
    }
    std::string encode()const{
        validate();
        const auto meta=LibraryJson{{"source",source.json()},{"blob",blob},{"decoder",decoder},
            {"total",total},{"start",start},{"overview",overview}}.dump();
        if(meta.size()>512*1024)throw std::runtime_error("Saved preview metadata exceeds limit.");
        stateio::BinaryWriter out;out.text("PRAPPY_SAMPLE_PREVIEW_1");out.number(meta.size());out.bytes+=meta;out.number(pcm.size());
        static_assert(std::endian::native==std::endian::little&&sizeof(float)==4);
        if(!pcm.empty())out.bytes.append(reinterpret_cast<const char*>(pcm.data()),pcm.size()*sizeof(float));
        out.number(stateio::checksum(out.bytes));return out.bytes;
    }
    static Preview decode(const std::string& bytes){
        if(bytes.size()<8||bytes.size()>MaxBytes)throw std::runtime_error("Invalid saved preview size.");
        stateio::BinaryReader tail{std::string_view(bytes).substr(bytes.size()-8)};
        if(tail.number()!=stateio::checksum(std::string_view(bytes).substr(0,bytes.size()-8)))throw std::runtime_error("Saved preview checksum differs.");
        stateio::BinaryReader in{std::string_view(bytes).substr(0,bytes.size()-8)};
        if(in.text()!="PRAPPY_SAMPLE_PREVIEW_1")throw std::runtime_error("Unsupported saved preview version.");
        const auto length=in.count(512*1024);
        if(length>in.bytes.size()-in.offset)throw std::runtime_error("Truncated saved preview metadata.");
        const auto meta=LibraryJson::parse(in.bytes.substr(in.offset,length));in.offset+=length;
        Preview result{SourceRef::parse(meta.at("source")),meta.at("blob"),meta.at("decoder"),meta.at("total"),meta.at("start"),meta.at("overview"),{}};
        const auto samples=in.count(static_cast<size_t>(MaxFrames)*2);
        if(samples*sizeof(float)!=in.bytes.size()-in.offset)throw std::runtime_error("Truncated saved preview PCM.");
        result.pcm.resize(samples);if(samples)std::memcpy(result.pcm.data(),in.bytes.data()+in.offset,samples*sizeof(float));
        in.offset+=samples*sizeof(float);in.finish();result.validate();return result;
    }
    static std::string read(const std::filesystem::path& path){
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if(!input)throw std::runtime_error("Saved preview is unavailable.");
        const auto size=input.tellg();
        if(size<0||static_cast<uint64_t>(size)>MaxBytes)throw std::runtime_error("Saved preview exceeds its memory limit.");
        std::string bytes(static_cast<size_t>(size),'\0');input.seekg(0);
        if(!input.read(bytes.data(),static_cast<std::streamsize>(bytes.size())))throw std::runtime_error("Saved preview read failed.");
        return bytes;
    }
};

// Disk reads, checksums and durable writes never run on the window thread.
// One selected read and one coalesced write keep memory and queued work bounded.
class PreviewStore {
public:
    struct Result {uint64_t ticket=0;std::optional<Preview> value;};
    using Reader=std::function<std::string(const std::filesystem::path&)>;
private:
    std::filesystem::path path;
    std::function<void()> notify;
    Reader reader;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::optional<uint64_t> requested;
    std::optional<Preview> pending;
    std::optional<Result> completed;
    std::jthread worker;
    void run(std::stop_token stop){
        for(;;){
            std::optional<uint64_t> read;
            std::optional<Preview> write;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,stop,[&]{return requested.has_value()||pending.has_value();});
                if(requested&&!stop.stop_requested()){read=requested;requested.reset();}
                else if(pending){write=std::move(pending);pending.reset();}
                else break;
            }
            if(read){
                Result result{*read,{}};
                try{result.value=Preview::decode(reader(path));}
                catch(const std::exception&){try{result.value=Preview::decode(reader(path.wstring()+L".bak"));}catch(const std::exception&){}}
                {std::lock_guard lock(mutex);completed=std::move(result);}
                if(notify)notify();
            }
            if(write)try{stateio::write(path,write->encode());}catch(const std::exception& e){OutputDebugStringA(e.what());}
        }
    }
public:
    PreviewStore(std::filesystem::path file,std::function<void()> wakeUI={},Reader read=Preview::read)
        :path(std::move(file)),notify(std::move(wakeUI)),reader(std::move(read)),worker([this](std::stop_token stop){run(stop);}){}
    ~PreviewStore(){worker.request_stop();wake.notify_all();worker.join();}
    void request(uint64_t ticket){std::lock_guard lock(mutex);requested=ticket;wake.notify_all();}
    void save(Preview value){std::lock_guard lock(mutex);pending=std::move(value);wake.notify_all();}
    std::optional<Result> collect(){std::lock_guard lock(mutex);auto value=std::move(completed);completed.reset();return value;}
};
}
