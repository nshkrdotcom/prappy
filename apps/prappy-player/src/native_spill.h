#pragma once
#include "native_session.h"
#include <windows.h>
#include <compressapi.h>
#include <bcrypt.h>
#include <filesystem>

#pragma comment(lib,"cabinet.lib")
namespace sampling {
// Selection-only compressed bytes. No audio codec, file-format conversion or song cache.
class CompressedSpill final:public PcmBacking {
    static constexpr uint64_t globalLimit=8ULL*1024*1024*1024,reserve=1024ULL*1024*1024;
    static inline std::mutex quotaMutex;
    static inline uint64_t charged=0;
    struct Codec {
        COMPRESSOR_HANDLE compressor=nullptr;DECOMPRESSOR_HANDLE decompressor=nullptr;
        mutable std::mutex mutex;PcmBudget::Permit scratch;
        explicit Codec(PcmBudget budget):scratch(budget.reserve(8*1024*1024)){
            if(!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&compressor)||
               !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&decompressor)){
                if(compressor)CloseCompressor(compressor);if(decompressor)CloseDecompressor(decompressor);
                throw std::runtime_error("Windows lossless editing compression is unavailable.");
            }
        }
        ~Codec(){CloseCompressor(compressor);CloseDecompressor(decompressor);}
    };
    struct File {
        HANDLE handle=INVALID_HANDLE_VALUE;std::shared_ptr<Codec> codec;
        uint64_t raw=0,encoded=0,cost=0,limit;size_t records=0;std::wstring directory;
        File(std::shared_ptr<Codec> context,uint64_t ceiling):codec(std::move(context)),limit(ceiling){}
        ~File(){
            if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
            std::lock_guard lock(quotaMutex);charged-=cost;
        }
        void open(){
            if(handle!=INVALID_HANDLE_VALUE)return;
            std::array<wchar_t,MAX_PATH> folder{},path{};
            const auto count=GetTempPathW(static_cast<DWORD>(folder.size()),folder.data());
            if(!count||count>=folder.size()||!GetTempFileNameW(folder.data(),L"pcy",0,path.data()))throw std::runtime_error("Cannot create temporary editing storage.");
            directory=folder.data();handle=CreateFileW(path.data(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
            if(handle==INVALID_HANDLE_VALUE){DeleteFileW(path.data());throw std::runtime_error("Cannot open temporary editing storage.");}
        }
    };
    std::shared_ptr<File> file;
    static std::array<unsigned char,32> hash(std::span<const std::byte> bytes){
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot open editing PCM integrity check.");
        std::array<unsigned char,32> result{};
        const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
            static_cast<ULONG>(bytes.size()),result.data(),static_cast<ULONG>(result.size()));
        BCryptCloseAlgorithmProvider(algorithm,0);
        if(status<0)throw std::runtime_error("Editing PCM integrity check failed.");
        return result;
    }
    struct Chunk final:Stored {
        std::shared_ptr<File> owner;uint64_t offset;size_t raw;DWORD encoded;std::array<unsigned char,32> digest;
        Chunk(std::shared_ptr<File> f,uint64_t at,size_t size,DWORD compressed,std::array<unsigned char,32> sha)
            :owner(std::move(f)),offset(at),raw(size),encoded(compressed),digest(sha){}
        size_t bytes()const override{return raw;}
        void read(std::span<std::byte> output)const override{
            if(output.size()!=raw)throw std::runtime_error("Stored editing PCM destination differs.");
            std::lock_guard lock(owner->codec->mutex);std::vector<std::byte> packed(encoded);
            LARGE_INTEGER at;at.QuadPart=static_cast<LONGLONG>(offset);DWORD count=0;SIZE_T size=0;
            if(!SetFilePointerEx(owner->handle,at,nullptr,FILE_BEGIN)||
               !ReadFile(owner->handle,packed.data(),encoded,&count,nullptr)||count!=encoded||
               !Decompress(owner->codec->decompressor,packed.data(),packed.size(),output.data(),output.size(),&size)||size!=raw||hash(output)!=digest)
                throw std::runtime_error("Temporary editing PCM is incomplete or corrupt; retained work was not replaced.");
        }
    };
    CompressedSpill(std::shared_ptr<Codec> context,uint64_t limit):file(std::make_shared<File>(std::move(context),limit)){}
public:
    static uint64_t quotaUsed(){std::lock_guard lock(quotaMutex);return charged;}
    explicit CompressedSpill(PcmBudget budget,uint64_t limit=globalLimit):file(std::make_shared<File>(std::make_shared<Codec>(std::move(budget)),limit)){
        if(!limit||limit>globalLimit)throw std::runtime_error("Temporary editing storage limit must be between 1 byte and 8 GiB.");
    }
    std::shared_ptr<PcmBacking> fork()const override{return std::shared_ptr<PcmBacking>(new CompressedSpill(file->codec,file->limit));}
    std::shared_ptr<const Stored> store(std::span<const std::byte> bytes)override{
        if(bytes.empty()||bytes.size()>1024*1024)throw std::runtime_error("Temporary PCM writes must be at most 1 MiB.");
        std::lock_guard lock(file->codec->mutex);
        {std::lock_guard quota(quotaMutex);
            if(bytes.size()>file->limit-file->raw||bytes.size()>globalLimit-charged||file->records>=16384)
                throw std::runtime_error("Active editing audio reached its temporary storage limit; your retained audio is unchanged.");
        }
        file->open();ULARGE_INTEGER free{};
        if(!GetDiskFreeSpaceExW(file->directory.c_str(),&free,nullptr,nullptr)||free.QuadPart<reserve+bytes.size()+65536)
            throw std::runtime_error("Temporary editing storage needs 1 GiB free headroom.");
        std::vector<std::byte> packed(bytes.size()+65536);SIZE_T size=0;
        if(!Compress(file->codec->compressor,const_cast<std::byte*>(bytes.data()),bytes.size(),packed.data(),packed.size(),&size))
            throw std::runtime_error("Cannot compress authoritative editing PCM.");
        const auto digest=hash(bytes);auto result=std::make_shared<Chunk>(file,file->encoded,bytes.size(),static_cast<DWORD>(size),digest);
        LARGE_INTEGER at;at.QuadPart=static_cast<LONGLONG>(file->encoded);DWORD written=0;
        const auto cost=std::max<uint64_t>(bytes.size(),size)+sizeof(Chunk);
        {std::lock_guard quota(quotaMutex);
            if(cost>globalLimit-charged)throw std::runtime_error("Concurrent editing audio reached its temporary storage limit.");
            charged+=cost;file->cost+=cost;file->raw+=bytes.size();
        }
        if(!SetFilePointerEx(file->handle,at,nullptr,FILE_BEGIN)||!WriteFile(file->handle,packed.data(),static_cast<DWORD>(size),&written,nullptr)||written!=size){
            std::lock_guard quota(quotaMutex);charged-=cost;file->cost-=cost;file->raw-=bytes.size();
            throw std::runtime_error("Temporary editing write failed; resident audio was retained.");
        }
        file->encoded+=size;++file->records;return result;
    }
};
}
