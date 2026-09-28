#pragma once
#include "transport/json.hpp"
#include "swegca_architecture/content_observation_kernel.hpp"
#include <charconv>
#include <sys/stat.h>

namespace swegca::transport {
// Reconcile a recorded file predicate with its recorded measurement. This is
// not a new file read, proof of producer honesty, or natural-language relevance.
inline architecture::kernel::EvidenceOutcome measured_file_outcome(const Json& report,
    const Json& scope){
    const auto predicate=scope.at("predicate").string();
    if(predicate!="equal-file-bytes-v1"&&predicate!="different-file-bytes-v1")
        throw std::invalid_argument("file observation predicate mismatch");
    const auto flag=[](const Json& value){
        if(value.kind!=Json::Kind::boolean)throw std::invalid_argument("measurement flag must be boolean");
        return value.scalar=="true";
    };
    const auto number=[](const Json& value){
        const auto text=value.string();std::uint64_t n{};
        const auto r=std::from_chars(text.data(),text.data()+text.size(),n);
        if(text.empty()||r.ec!=std::errc{}||r.ptr!=text.data()+text.size())throw std::invalid_argument("invalid measurement integer");
        return n;
    };
    const auto hash=[](const Json& value){
        const auto text=value.string();
        if(text.size()!=64)throw std::invalid_argument("invalid measurement digest");
        for(char c:text)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))throw std::invalid_argument("invalid measurement digest");
        return text;
    };
    const auto& measurement=report.at("measurement");
    bool complete=flag(measurement.at("complete")),stable=flag(measurement.at("stable"));
    const auto& error=measurement.at("ioError");
    if(error.kind!=Json::Kind::number)throw std::invalid_argument("invalid measurement I/O status");
    complete=complete&&error.scalar=="0";
    for(const auto name:{"left","right"}){
        const auto& file=measurement.at(name);
        if(file.at("path").string()!=scope.at(name).string())throw std::invalid_argument("measurement operand mismatch");
        if(!complete)continue;
        const auto& before=file.at("before");const auto& after=file.at("after");
        if(before.kind!=Json::Kind::object||after.kind!=Json::Kind::object)
            throw std::invalid_argument("complete measurement requires file versions");
        if((number(before.at("mode"))&S_IFMT)!=S_IFREG||number(file.at("readBytes"))!=number(before.at("size")))
            throw std::invalid_argument("measurement is not a complete regular file read");
        for(const auto key:{"device","inode","size","mode","mtimeSeconds","mtimeNanos","ctimeSeconds","ctimeNanos"})
            stable=stable&&before.at(key).string()==after.at(key).string();
    }
    bool equal=false;
    if(complete){
        const auto& left=measurement.at("left");const auto& right=measurement.at("right");
        // A complete measurement requires both valid digests even when size
        // already disproves equality. Do not short-circuit structural evidence
        // validation into support for the opposite predicate.
        const auto left_digest=hash(left.at("digest")),right_digest=hash(right.at("digest"));
        equal=number(left.at("readBytes"))==number(right.at("readBytes"))&&left_digest==right_digest;
    }
    return architecture::kernel::to_outcome(architecture::kernel::observe_content_relation(complete,stable,equal,predicate=="equal-file-bytes-v1"));
}
} // namespace swegca::transport
