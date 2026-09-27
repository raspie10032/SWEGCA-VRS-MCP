#include "vrs/runtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace swegca::vrs;
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;

static DigestBytes identity(unsigned value){DigestBytes result{};result[0]=std::byte(value);return result;}

// Functional diagnostic, not a performance benchmark or a passing test gate.
// Exit 2 means that reopening the same active session changed continuation.
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"swegca-continuation-XXXXXX").string();
    if(!::mkdtemp(pattern.data()))return 1;
    const std::filesystem::path root(pattern);
    MemoryBudget memory(16<<20);
    EvidencePolicy policy;policy.axis_count=1;
    RuntimeConfig config{identity(99),policy,1,65536,8192,65536};
    const std::string purpose="Keep the saved result unchanged.";
    const std::string report="The saved result changed.";
    const std::string followup="Continue from that result.";
    ExperienceLocation input,observation,before,after;
    unsigned before_kind=0,after_kind=0;
    {
        auto runtime=Runtime::create(root,config,memory);
        runtime.start_session(identity(1),"continuation");
        input=runtime.receive({0,0,"continuation","user","text/plain",std::as_bytes(std::span(purpose))},7,0).recorded.original;
        EvidenceObservation value;value.source=identity(2);value.producer=identity(3);
        value.observed_at=1;value.outcome=EvidenceOutcome::refute;
        observation=runtime.observe_input_scope(input,"saved result",
            {1,1,"continuation","tool","text/plain",std::as_bytes(std::span(report))},value,7,1).original;
        auto recalled=runtime.input("text/plain",std::as_bytes(std::span(purpose)));
        auto parent=runtime.replay(recalled,0);
        auto related=runtime.related(parent);
        auto cognition=runtime.cognize(related,7,1);
        if(!cognition||cognition->replayed.location()!=observation)return 1;
        auto continued=runtime.input("text/plain",std::as_bytes(std::span(followup)));
        before_kind=static_cast<unsigned>(continued.key_kind());
        if(const auto selected=runtime.select_replay(continued))before=continued.matches()[*selected].original;
        // No end event: restart must not publish or merge this session.
        if(runtime.work(7,1)!=0)return 1;
    }
    {
        auto runtime=Runtime::open(root,config,memory);runtime.resume_session(identity(1));
        auto continued=runtime.input("text/plain",std::as_bytes(std::span(followup)));
        after_kind=static_cast<unsigned>(continued.key_kind());
        if(const auto selected=runtime.select_replay(continued))after=continued.matches()[*selected].original;
        if(runtime.main().graph().generation()!=0||runtime.work(7,1)!=0)return 1;
    }
    const bool preserved=before==after&&before==observation;
    std::printf("{\"continuationPreserved\":%s,\"beforeKeyKind\":%u,\"afterKeyKind\":%u,"
        "\"beforeWasObservation\":%s,\"afterWasObservation\":%s,\"afterWasInput\":%s,"
        "\"mainGeneration\":0,\"modelCalls\":0}\n",
        preserved?"true":"false",before_kind,after_kind,before==observation?"true":"false",
        after==observation?"true":"false",after==input?"true":"false");
    std::filesystem::remove_all(root);
    return preserved?0:2;
}
