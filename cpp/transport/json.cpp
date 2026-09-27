#include "transport/json.hpp"
#include <stdexcept>
#include <ostream>
#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#if defined(__SSE2__) && !defined(SWEGCA_JSON_SCALAR_ONLY)
#include <emmintrin.h>
#endif

namespace swegca::transport {
const Json* Json::find(std::string_view key) const noexcept {
    if(kind!=Kind::object)return nullptr;
    for(std::size_t i=0;i<keys.size();++i)if(keys[i]==key)return &values[i];
    return nullptr;
}
const Json& Json::at(std::string_view key) const {
    const auto* value=find(key);if(!value)throw std::invalid_argument("missing JSON field");return *value;
}
std::string_view Json::string() const {if(kind!=Kind::string)throw std::invalid_argument("expected JSON string");return scalar;}
namespace {
[[noreturn]] void invalid(){throw std::invalid_argument("invalid JSON syntax");}
// Only byte classification; Unicode and escape semantics remain below.
// Full-width loads are used only while 16 bytes remain in the supplied view.
template<bool Ordinary> std::size_t prefix(std::string_view text) noexcept {
    std::size_t pos=0;
#if defined(__SSE2__) && !defined(SWEGCA_JSON_SCALAR_ONLY)
    while(text.size()-pos>=16){
        const auto bytes=_mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data()+pos));
        unsigned mask;
        if constexpr(Ordinary){
            const auto control=_mm_cmpeq_epi8(_mm_and_si128(bytes,_mm_set1_epi8(char(0xe0))),_mm_setzero_si128());
            const auto quote=_mm_cmpeq_epi8(bytes,_mm_set1_epi8('"'));
            const auto slash=_mm_cmpeq_epi8(bytes,_mm_set1_epi8('\\'));
            mask=static_cast<unsigned>(_mm_movemask_epi8(_mm_or_si128(control,_mm_or_si128(quote,slash))));
        }else mask=static_cast<unsigned>(_mm_movemask_epi8(bytes));
        if(mask)return pos+std::countr_zero(mask);
        pos+=16;
    }
#endif
    while(pos<text.size()){
        const auto c=static_cast<unsigned char>(text[pos]);
        if constexpr(Ordinary){if(c<32||c=='"'||c=='\\')break;}
        else if(c>=128)break;
        ++pos;
    }
    return pos;
}
void utf8(std::string_view text) {
    for(std::size_t i=0;i<text.size();){
        i+=prefix<false>(text.substr(i));if(i==text.size())break;
        const unsigned c=static_cast<unsigned char>(text[i++]);
        unsigned n=0,value=0,min=0;
        if(c>=0xc2&&c<=0xdf){n=1;value=c&31;min=128;}
        else if(c>=0xe0&&c<=0xef){n=2;value=c&15;min=2048;}
        else if(c>=0xf0&&c<=0xf4){n=3;value=c&7;min=65536;}
        else invalid();
        if(n>text.size()-i)invalid();
        for(unsigned j=0;j<n;++j){const unsigned d=static_cast<unsigned char>(text[i++]);if((d&0xc0)!=0x80)invalid();value=(value<<6)|(d&63);}
        if(value<min||value>0x10ffff||(value>=0xd800&&value<=0xdfff))invalid();
    }
}
class Parser {
public:
    Parser(std::string_view text,std::pmr::memory_resource& memory,std::size_t depth):text_(text),memory_(memory),depth_(depth){utf8(text);}
    Json parse(){auto result=value(0);space();if(pos_!=text_.size())invalid();return result;}
private:
    std::string_view text_;std::pmr::memory_resource& memory_;std::size_t depth_,pos_=0;
    char peek()const{return pos_<text_.size()?text_[pos_]:'\0';}
    char take(){if(pos_==text_.size())invalid();return text_[pos_++];}
    void space(){while(peek()==' '||peek()=='\t'||peek()=='\r'||peek()=='\n')++pos_;}
    bool eat(char c){if(peek()!=c)return false;++pos_;return true;}
    unsigned hex4(){unsigned n=0;for(unsigned i=0;i<4;++i){char c=take();n<<=4;if(c>='0'&&c<='9')n|=c-'0';else if(c>='a'&&c<='f')n|=c-'a'+10;else if(c>='A'&&c<='F')n|=c-'A'+10;else invalid();}return n;}
    static void codepoint(std::pmr::string& out,unsigned n){
        if(n<128)out+=char(n);
        else if(n<2048){out+=char(0xc0|(n>>6));out+=char(0x80|(n&63));}
        else if(n<65536){out+=char(0xe0|(n>>12));out+=char(0x80|((n>>6)&63));out+=char(0x80|(n&63));}
        else{out+=char(0xf0|(n>>18));out+=char(0x80|((n>>12)&63));out+=char(0x80|((n>>6)&63));out+=char(0x80|(n&63));}
    }
    std::pmr::string string(){
        if(take()!='"')invalid();
        std::pmr::string out(&memory_);
        for(;;){
            const auto begin=pos_;
            pos_+=prefix<true>(text_.substr(pos_));
            out.append(text_.substr(begin,pos_-begin));
            const char c=take();if(c=='"')return out;if(static_cast<unsigned char>(c)<32)invalid();
            switch(take()){
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{unsigned n=hex4();if(n>=0xd800&&n<=0xdbff){if(take()!='\\'||take()!='u')invalid();unsigned low=hex4();if(low<0xdc00||low>0xdfff)invalid();n=0x10000+((n-0xd800)<<10)+(low-0xdc00);}else if(n>=0xdc00&&n<=0xdfff)invalid();codepoint(out,n);break;}
            default:invalid();}
        }
    }
    Json value(std::size_t depth){
        if(depth>depth_)throw std::length_error("JSON nesting limit");
        space();Json out(&memory_);
        const auto c=peek();
        if(c=='"'){out.kind=Json::Kind::string;out.scalar=string();return out;}
        if(c=='['||c=='{'){
            ++pos_;const bool object=c=='{';out.kind=object?Json::Kind::object:Json::Kind::array;space();
            const char close=object?'}':']';if(eat(close))return out;
            for(;;){space();if(object){auto key=string();for(const auto& k:out.keys)if(k==key)throw std::invalid_argument("duplicate JSON member");space();if(!eat(':'))invalid();out.keys.push_back(std::move(key));}
                out.values.push_back(value(depth+1));space();if(eat(close))return out;if(!eat(','))invalid();}
        }
        for(std::string_view literal:{"null","true","false"})if(text_.substr(pos_,literal.size())==literal){pos_+=literal.size();out.kind=literal=="null"?Json::Kind::null:Json::Kind::boolean;out.scalar=literal;return out;}
        const auto start=pos_;eat('-');
        if(!eat('0')){if(peek()<'1'||peek()>'9')invalid();while(peek()>='0'&&peek()<='9')++pos_;}
        if(eat('.')){if(peek()<'0'||peek()>'9')invalid();while(peek()>='0'&&peek()<='9')++pos_;}
        if(eat('e')||eat('E')){if(!eat('+'))eat('-');if(peek()<'0'||peek()>'9')invalid();while(peek()>='0'&&peek()<='9')++pos_;}
        out.kind=Json::Kind::number;out.scalar=text_.substr(start,pos_-start);return out;
    }
};
std::size_t add_size(std::size_t left,std::size_t right){
    if(right>std::numeric_limits<std::size_t>::max()-left)throw std::length_error("JSON size overflow");
    return left+right;
}
std::size_t quoted_size(std::string_view text){
    utf8(text);auto size=add_size(text.size(),2);
    std::size_t pos=0;
    while(pos<text.size()){
        pos+=prefix<true>(text.substr(pos));if(pos==text.size())break;
        size=add_size(size,static_cast<unsigned char>(text[pos++])<32?5:1);
    }
    return size;
}
std::size_t encoded_size(const Json& value){
    switch(value.kind){
    case Json::Kind::null:return 4;
    case Json::Kind::boolean:case Json::Kind::number:return value.scalar.size();
    case Json::Kind::string:return quoted_size(value.scalar);
    case Json::Kind::array:case Json::Kind::object:{
        const bool object=value.kind==Json::Kind::object;
        if(object&&value.keys.size()!=value.values.size())invalid();
        std::size_t size=2;
        for(std::size_t i=0;i<value.values.size();++i){
            if(i)size=add_size(size,1);
            if(object)size=add_size(size,add_size(quoted_size(value.keys[i]),1));
            size=add_size(size,encoded_size(value.values[i]));
        }
        return size;
    }
    }
    invalid();
}
// Size validation has checked every source string before output allocation.
void quote(std::pmr::string& out,std::string_view text){
    constexpr char digits[]="0123456789abcdef";out+='"';
    std::size_t pos=0;
    while(pos<text.size()){
        const auto count=prefix<true>(text.substr(pos));out.append(text.substr(pos,count));pos+=count;
        if(pos==text.size())break;
        const auto c=static_cast<unsigned char>(text[pos++]);
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else{out+="\\u00";out+=digits[c>>4];out+=digits[c&15];}
    }
    out+='"';
}
void encode(std::pmr::string& out,const Json& value){
    switch(value.kind){
    case Json::Kind::null:out+="null";return;
    case Json::Kind::boolean:case Json::Kind::number:out+=value.scalar;return;
    case Json::Kind::string:quote(out,value.scalar);return;
    case Json::Kind::array:case Json::Kind::object:{
        const bool object=value.kind==Json::Kind::object;if(object&&value.keys.size()!=value.values.size())invalid();out+=object?'{':'[';
        for(std::size_t i=0;i<value.values.size();++i){if(i)out+=',';if(object){quote(out,value.keys[i]);out+=':';}encode(out,value.values[i]);}out+=object?'}':']';return;}
    }
}
}
static void write_escaped_content(std::ostream& out,std::string_view text){
    constexpr char digits[]="0123456789abcdef";
    std::size_t pos=0;
    while(pos<text.size()){
        const auto count=prefix<true>(text.substr(pos));
        out.write(text.data()+pos,static_cast<std::streamsize>(count));pos+=count;
        if(pos==text.size())break;
        const auto c=static_cast<unsigned char>(text[pos++]);
        if(c=='"'||c=='\\'){const char escaped[]{'\\',char(c)};out.write(escaped,2);}
        else{const char escaped[]{'\\','u','0','0',digits[c>>4],digits[c&15]};out.write(escaped,6);}
    }
}
void write_json_string_content(std::ostream& out,std::string_view text){
    utf8(text);write_escaped_content(out,text);
}
void write_json_string(std::ostream& out,std::string_view text){
    utf8(text);out.put('"');write_escaped_content(out,text);out.put('"');
}
void write_json_hex(std::ostream& out,std::span<const std::byte> content){
    constexpr char digits[]="0123456789abcdef";std::array<char,4096> buffer;
    while(!content.empty()){
        const auto count=std::min(content.size(),buffer.size()/2);
        for(std::size_t i=0;i<count;++i){const auto c=std::to_integer<unsigned>(content[i]);buffer[2*i]=digits[c>>4];buffer[2*i+1]=digits[c&15];}
        out.write(buffer.data(),static_cast<std::streamsize>(count*2));content=content.subspan(count);
    }
}
Json parse_json(std::string_view text,std::pmr::memory_resource& memory,std::size_t depth){return Parser(text,memory,depth).parse();}
std::pmr::string encode_json(const Json& value,std::pmr::memory_resource& memory){std::pmr::string out(&memory);out.reserve(encoded_size(value));encode(out,value);return out;}
void append_json(std::pmr::string& destination,const Json& value,std::size_t suffix_capacity){
    destination.reserve(add_size(add_size(destination.size(),encoded_size(value)),suffix_capacity));encode(destination,value);
}
std::pmr::string quote_json(std::string_view text,std::pmr::memory_resource& memory){std::pmr::string out(&memory);out.reserve(quoted_size(text));quote(out,text);return out;}
} // namespace swegca::transport
