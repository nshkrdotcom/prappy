#include "native_session.h"
#include "paged_vector.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace prepared;
using namespace sampling;

void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class Action> void rejects(Action&& action){
    bool thrown=false;
    try{action();}catch(const std::exception&){thrown=true;}
    require(thrown,"Expected lifecycle operation to reject invalid input");
}

// A deterministic quota-accounting backing. The code under test is the real
// NativeSession/Clip; only its platform-specific persistence boundary is faked.
struct CountingBacking final:PcmBacking {
    struct Ledger{size_t charged=0,attempts=0,failOn=0,files=0;};
    struct File {
        std::shared_ptr<Ledger> ledger;
        size_t bytes=0;
        explicit File(std::shared_ptr<Ledger> state):ledger(std::move(state)){++ledger->files;}
        ~File(){ledger->charged-=bytes;--ledger->files;}
    };
    struct Chunk final:Stored {
        std::shared_ptr<File> file;
        std::vector<std::byte> payload;
        Chunk(std::shared_ptr<File> owner,std::span<const std::byte> source)
            :file(std::move(owner)),payload(source.begin(),source.end()){}
        size_t bytes()const override{return payload.size();}
        void read(std::span<std::byte> target)const override{
            if(target.size()!=payload.size())throw std::runtime_error("Injected stored byte mismatch");
            std::copy(payload.begin(),payload.end(),target.begin());
        }
    };
    std::shared_ptr<Ledger> ledger;
    std::shared_ptr<File> file;
    explicit CountingBacking(std::shared_ptr<Ledger> state)
        :ledger(std::move(state)),file(std::make_shared<File>(ledger)){}
    std::shared_ptr<PcmBacking> fork()const override{return std::make_shared<CountingBacking>(ledger);}
    std::shared_ptr<const Stored> store(std::span<const std::byte> bytes)override{
        ++ledger->attempts;
        if(ledger->failOn==ledger->attempts)throw std::runtime_error("Injected spill full / interrupted write");
        auto value=std::make_shared<Chunk>(file,bytes);
        ledger->charged+=bytes.size();file->bytes+=bytes.size();
        return value;
    }
};
std::vector<std::byte> bytes(size_t size){
    std::vector<std::byte> result(size);
    for(size_t i=0;i<size;++i)result[i]=static_cast<std::byte>((i*37+(i>>8))&0xff);
    return result;
}
std::shared_ptr<const Block> block(const NativeTimeline& timeline,Frame start,Frame count,std::span<const std::byte> input){
    return std::make_shared<const Block>(timeline,start,count,input);
}

void waveformArithmeticRespectsNativeClock(){
    require(prepared::ceilDivPositive(INT64_MAX,4096)==INT64_MAX/4096+1,
        "Waveform bin count overflowed a valid 64-bit native clock");
    require(prepared::ceilDivPositive(8193,4096)==3&&prepared::ceilDivPositive(0,7)==0,
        "Waveform bin count rounded incorrectly");
    rejects([&]{(void)prepared::ceilDivPositive(123,0);});
    rejects([&]{(void)prepared::ceilDivPositive(-1,4096);});
}
void budgetExhaustionAndRetry(){
    PcmBudget budget(64);
    auto large=budget.reserve(60ULL*1024*1024);
    rejects([&]{(void)budget.reserve(8ULL*1024*1024);});
    require(budget.used()==60ULL*1024*1024,"Rejected reservation left budget charged");
    large.reset();
    auto retry=budget.reserve(8ULL*1024*1024);
    require(budget.used()==8ULL*1024*1024,"Released budget could not be reserved again");
    retry.reset();require(budget.used()==0,"Retry permit failed to release quota");
}
void cancelledSaveCanRetryWithoutPinningExtraStorage(){
    PcmBudget budget(64);
    auto ledger=std::make_shared<CountingBacking::Ledger>();
    const NativeTimeline clock{48000,1,262144,SampleType::Int32};
    auto original=bytes(1024*1024);
    std::shared_ptr<const NativeSession::Clip> saved;
    size_t decoded=0;
    {
        NativeSession session(clock,"key","timeline",1,budget);
        session.preserve(0,clock.frames,std::make_shared<CountingBacking>(ledger));
        saved=session.clip(0,clock.frames,[&](Frame start,Frame count,std::stop_token){
            ++decoded;
            auto pcm=block(clock,start,count,std::span(original).subspan(static_cast<size_t>(start)*4,static_cast<size_t>(count)*4));
            return std::make_shared<const NativeSession::Buffer>(NativeSession::Buffer{pcm,budget.reserve(pcm->bytes().size()),"reader"});
        });
        std::stop_source cancel;
        size_t streamed=0;
        rejects([&]{saved->each([&](std::span<const std::byte> part){
            streamed+=part.size();cancel.request_stop();
        },cancel.get_token());});
        require(streamed==65536&&decoded==1,"Streaming cancellation did not stop after the current bounded chunk");
        require(ledger->charged==original.size()&&budget.used()==0,"Cancelled upload leaked scratch PCM or lost retryable bytes");
    }
    std::vector<std::byte> replay;
    saved->each([&](std::span<const std::byte> part){replay.insert(replay.end(),part.begin(),part.end());});
    require(replay==original&&decoded==1,"Cancelled save replayed incorrect PCM or re-decoded immutable bytes");
    saved.reset();
    require(ledger->charged==0&&ledger->files==0&&budget.used()==0,
        "Completed cancellation retry held backing file or PCM permit");
}
void failedSpillDoesNotPinQuotaOrEvict(){
    PcmBudget budget(64);
    auto ledger=std::make_shared<CountingBacking::Ledger>();
    const NativeTimeline clock{48000,1,524288,SampleType::Int32};
    const auto input=bytes(2*1024*1024);
    {
        NativeSession session(clock,"key","timeline",1,budget);
        session.preserve(0,524288,std::make_shared<CountingBacking>(ledger));
        for(Frame at=0;at<524288;at+=262144){
            const auto pcm=block(clock,at,262144,std::span(input).subspan(static_cast<size_t>(at)*4,1024*1024));
            session.adopt(pcm,budget.reserve(pcm->bytes().size()),"key","timeline",1,"original");
        }
        require(budget.used()==input.size(),"Resident PCM reservation is wrong");
        ledger->failOn=2;
        rejects([&]{session.retain(524288,524288);});
        require(ledger->charged==0,"Failed multi-chunk eviction pinned unreachable storage quota");
        require(budget.used()==input.size()&&session.covers(0,524288),"Failed eviction dropped native PCM");
        require(session.find(0)&&session.find(262144),"Failed eviction damaged resident data");
        ledger->failOn=0;
        session.retain(524288,524288);
        require(budget.used()==0&&ledger->charged==input.size(),"Successful eviction did not transfer ownership");
        auto saved=session.clip(0,524288);
        std::vector<std::byte> read;
        saved->each([&](std::span<const std::byte> part){read.insert(read.end(),part.begin(),part.end());});
        require(read==input,"Eviction/recovery changed the original PCM bits");
    }
    require(ledger->charged==0&&ledger->files==0,"Retiring session/snapshot retained storage");
    require(budget.used()==0,"Retiring session/snapshot retained PCM permits");
}
void lazySaveRetryReusesValidChunksAndReleasesQuota(){
    PcmBudget budget(64);
    auto ledger=std::make_shared<CountingBacking::Ledger>();
    const NativeTimeline clock{96000,1,524288,SampleType::Int32};
    const auto input=bytes(2*1024*1024);
    std::shared_ptr<const NativeSession::Clip> saved;
    size_t reads=0;
    {
        NativeSession session(clock,"key","timeline",4,budget);
        session.preserve(0,clock.frames,std::make_shared<CountingBacking>(ledger));
        saved=session.clip(0,clock.frames,[&](Frame start,Frame count,std::stop_token){
            ++reads;
            const auto pcm=block(clock,start,count,std::span(input).subspan(static_cast<size_t>(start)*4,static_cast<size_t>(count)*4));
            return std::make_shared<const NativeSession::Buffer>(NativeSession::Buffer{pcm,budget.reserve(pcm->bytes().size()),"reopened"});
        });
        ledger->failOn=2;
        rejects([&]{saved->each([](std::span<const std::byte>){});});
        require(ledger->charged==1024*1024&&reads==2,"Interrupted save lost completed gap or retained failed charge");
        ledger->failOn=0;
        std::vector<std::byte> replay;
        saved->each([&](std::span<const std::byte> part){replay.insert(replay.end(),part.begin(),part.end());});
        require(replay==input&&reads==3,"Save retry did not reuse the immutable first chunk");
        require(ledger->charged==input.size(),"Save retry double-charged stored bytes");
        require(budget.used()==0,"Streaming save failed to return all scratch/reader permits");
    }
    // Immutable upload survives session teardown until the request is retired.
    require(ledger->charged==input.size(),"Snapshot lost saved backing with the session");
    std::vector<std::byte> replay;
    saved->each([&](std::span<const std::byte> part){replay.insert(replay.end(),part.begin(),part.end());});
    require(replay==input,"Snapshot cannot be replayed after source closes");
    saved.reset();
    require(ledger->charged==0&&ledger->files==0,"Finished immutable save retained scratch storage");
}
void wrongBudgetAndOverlapAreRejectedWithoutRetention(){
    PcmBudget budget(64),other(64);
    const NativeTimeline clock{48000,1,8,SampleType::Int32};
    const auto input=bytes(32);
    NativeSession session(clock,"key","timeline",9,budget);
    auto sample=block(clock,0,8,input);
    rejects([&]{session.adopt(sample,other.reserve(32),"key","timeline",9,"bad");});
    require(other.used()==0&&budget.used()==0,"Foreign memory reservation survived rejected adopt");
    auto accepted=session.adopt(sample,budget.reserve(32),"key","timeline",9,"good");
    rejects([&]{session.adopt(sample,budget.reserve(32),"key","timeline",9,"good");});
    require(session.frames()==8&&budget.used()==32,"Duplicate native response was retained");
    require(accepted->decoderEpoch=="good","Accepted decoder identity changed");
    session.retain(8,8);accepted.reset();
    require(budget.used()==0,"Evicted response retained a memory reservation");
}
void coldSpanCorruptionRejectsBeforeBudgetedRead(){
    struct WrongSize final:PcmBacking::Stored{
        size_t bytes()const override{return 2;}
        void read(std::span<std::byte>)const override{throw std::runtime_error("Read should never be attempted");}
    };
    // Feed a corrupt stored span to the real Clip consumer to exercise the
    // bounds check before a pointer can be computed from untrusted metadata.
    NativeSession::Clip selection;
    selection.source={48000,1,8,SampleType::Int32};
    selection.first=0;selection.last=8;
    selection.segments.push_back({{},std::make_shared<WrongSize>(),0,8,0,"source"});
    rejects([&]{selection.each([](std::span<const std::byte>){});});
}
struct PageElement {
    int value=0;
    static inline bool failMove=false;
    static inline bool failCopy=false;
    explicit PageElement(int v):value(v){}
    PageElement(const PageElement& other):value(other.value){
        if(failCopy)throw std::runtime_error("Injected page clone failure");
    }
    PageElement(PageElement&& other):value(other.value){
        if(failMove)throw std::runtime_error("Injected page append failure");
    }
};
void failedPagedAppendPreservesSnapshotAndPageIndex(){
    PagedVector<PageElement,2> values;
    PageElement::failMove=true;
    rejects([&]{values.push_back(PageElement{1});});
    PageElement::failMove=false;
    require(values.empty(),"Failed first append left a ghost page");
    values.push_back(PageElement{2});
    auto snapshot=values;
    PageElement::failCopy=true;
    rejects([&]{values.push_back(PageElement{3});});
    PageElement::failCopy=false;
    require(values.size()==1&&snapshot.size()==1&&values[0].value==2,
        "Failed clone damaged shared snapshot or count");
    values.push_back(PageElement{3});
    require(values.size()==2&&values[1].value==3&&snapshot.size()==1&&snapshot[0].value==2,
        "Successful copy-on-write append changed older snapshot");
    PageElement::failMove=true;
    rejects([&]{values.push_back(PageElement{4});});
    PageElement::failMove=false;
    require(values.size()==2,"Failed new-page append published a ghost page");
    values.push_back(PageElement{4});
    require(values.size()==3&&values[2].value==4,
        "Retry after new-page allocation failure used the wrong page");
}

}
int main(){
    waveformArithmeticRespectsNativeClock();
    budgetExhaustionAndRetry();
    cancelledSaveCanRetryWithoutPinningExtraStorage();
    failedSpillDoesNotPinQuotaOrEvict();
    lazySaveRetryReusesValidChunksAndReleasesQuota();
    wrongBudgetAndOverlapAreRejectedWithoutRetention();
    coldSpanCorruptionRejectsBeforeBudgetedRead();
    failedPagedAppendPreservesSnapshotAndPageIndex();
    std::cout<<"PASS native ownership: atomic spill rollback, retry, immutable upload lifetime, provenance, overlapping replies, page snapshot failure recovery, quota and PCM cleanup\n";
}
