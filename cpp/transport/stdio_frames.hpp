#pragma once
#include <array>
#include <cerrno>
#include <cstring>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace swegca::transport {
// Exclusive blocking descriptor, borrowed from the owner. EOF is a transport
// boundary only. Retain read-ahead bytes for the next message, including after
// draining an oversized frame. No experience or session decision lives here.
class StdioFrames final {
public:
    class ReadError final : public std::system_error {
    public:
        explicit ReadError(int code):std::system_error(code,std::generic_category(),"read MCP frame"){}
    };
    StdioFrames(int fd,std::size_t limit,std::pmr::memory_resource& memory)
        :fd_(fd),limit_(limit),memory_(memory){
        if(!limit)throw std::invalid_argument("frame limit must be positive");
    }
    StdioFrames(const StdioFrames&)=delete;
    StdioFrames& operator=(const StdioFrames&)=delete;
    std::pmr::string next(bool& eof){
        eof=false;std::pmr::string line(&memory_);bool overflow=false;
        for(;;){
            if(begin_==end_){
                ssize_t count;
                do{count=::read(fd_,staging_.data(),staging_.size());}while(count<0&&errno==EINTR);
                if(count<0)throw ReadError(errno);
                if(!count){eof=true;if(overflow||!line.empty())throw std::invalid_argument("truncated MCP frame");return line;}
                begin_=0;end_=static_cast<std::size_t>(count);
            }
            const auto* start=staging_.data()+begin_;
            const auto* newline=static_cast<const char*>(std::memchr(start,'\n',end_-begin_));
            const auto count=newline?static_cast<std::size_t>(newline-start):end_-begin_;
            if(!overflow){
                if(count>limit_-line.size())overflow=true;
                else line.append(start,count);
            }
            begin_+=count;
            if(newline){
                ++begin_;
                if(overflow)throw std::length_error("frame exceeds configured limit");
                return line;
            }
        }
    }
private:
    int fd_;std::size_t limit_;std::pmr::memory_resource& memory_;
    std::array<char,4096> staging_;
    std::size_t begin_=0,end_=0;
};
} // namespace swegca::transport
