// CPU-only PAL code-object adapter: gfx1200/gfx1201 (NRPAL001 records), gfx11 and gfx10.3 (NRPAL002 records).
// No Vulkan or driver dependency.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include "nr_path_u8.hpp"   // UTF-8 paths (a game folder with any letters)
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nr::pal {
using Bytes = std::vector<uint8_t>;
inline void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
inline void range(const Bytes& b, size_t p, size_t n) { require(p <= b.size() && n <= b.size()-p, "truncated binary"); }
template<class T> T read(const Bytes& b, size_t p) { range(b,p,sizeof(T)); T t; std::memcpy(&t,b.data()+p,sizeof t); return t; }
template<class T> void write(Bytes& b, size_t p, T t) { range(b,p,sizeof t); std::memcpy(b.data()+p,&t,sizeof t); }
inline void word(Bytes& b, uint32_t v) { size_t p=b.size(); b.resize(p+4); write(b,p,v); }
inline uint64_t hash(const void* data, size_t n) {
    uint64_t h=1469598103934665603ull;
    const auto* p=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<n;++i) { h^=p[i]; h*=1099511628211ull; } return h;
}
inline Bytes load(const std::string& p) {
    std::ifstream f(nr::u8::path(p),std::ios::binary|std::ios::ate);
    require(bool(f),("cannot open binary: " + p).c_str()); auto n=f.tellg();
    require(n>0 && n<=64*1024*1024,"binary size outside limit");
    Bytes b(static_cast<size_t>(n)); f.seekg(0); f.read(reinterpret_cast<char*>(b.data()),n);
    require(bool(f),"cannot read binary"); return b;
}

// MessagePack nodes retain unknown scalar encodings. Maps/arrays are rebuilt,
// so fields may grow without overwriting adjacent metadata or ELF sections.
struct Value {
    enum Kind { Raw, UInt, String, Array, Map } kind=Raw;
    uint64_t u=0; std::string s; Bytes raw;
    std::vector<Value> items;
    std::vector<std::pair<std::string,Value>> fields;
    Value()=default;
    explicit Value(uint64_t v):kind(UInt),u(v) {}
    static Value boolean(bool b) { Value v; v.raw={uint8_t(b?0xc3:0xc2)}; return v; }
    Value& at(const std::string& key) {
        require(kind==Map,"expected metadata map");
        for(auto& f:fields) if(f.first==key) return f.second;
        throw std::runtime_error("missing metadata field: "+key);
    }
    const Value& at(const std::string& key) const { return const_cast<Value*>(this)->at(key); }
    void set(const std::string& key, Value v) {
        require(kind==Map,"expected metadata map");
        for(auto& f:fields) if(f.first==key) { f.second=std::move(v); return; }
        fields.emplace_back(key,std::move(v));
    }
    uint64_t number() const { require(kind==UInt,"expected unsigned metadata integer"); return u; }
};
inline uint64_t big(const Bytes& b,size_t& p,size_t n) {
    range(b,p,n); uint64_t v=0; while(n--) v=(v<<8)|b[p++]; return v;
}
inline Value decode(const Bytes& b,size_t& p,unsigned depth=0) {
    require(depth<40,"metadata nesting limit"); range(b,p,1); size_t start=p; unsigned c=b[p++]; Value v;
    size_t n=0; bool array=false,map=false,str=false;
    if(c<=0x7f) return Value(c);
    if(c>=0xa0 && c<=0xbf) {str=true;n=c&31;}
    else if((c&0xf0)==0x90) {array=true;n=c&15;}
    else if((c&0xf0)==0x80) {map=true;n=c&15;}
    else if(c>=0xcc && c<=0xcf) return Value(big(b,p,size_t(1)<<(c-0xcc)));
    else if(c==0xd9 || c==0xda || c==0xdb) {str=true;n=size_t(big(b,p,size_t(1)<<(c-0xd9)));}
    else if(c==0xdc || c==0xdd) {array=true;n=size_t(big(b,p,c==0xdc?2:4));}
    else if(c==0xde || c==0xdf) {map=true;n=size_t(big(b,p,c==0xde?2:4));}
    else {
        if(c==0xca)n=4; else if(c==0xcb)n=8;
        else if(c>=0xd0 && c<=0xd3)n=size_t(1)<<(c-0xd0);
        else if(c==0xc4 || c==0xc5 || c==0xc6)n=size_t(big(b,p,size_t(1)<<(c-0xc4)));
        else if(c>=0xd4 && c<=0xd8)n=(size_t(1)<<(c-0xd4))+1;
        else if(c==0xc7 || c==0xc8 || c==0xc9)n=size_t(big(b,p,size_t(1)<<(c-0xc7)))+1;
        else require(c>=0xe0 || c==0xc0 || c==0xc2 || c==0xc3,"unsupported MessagePack tag");
        range(b,p,n); p+=n; v.raw=Bytes(b.begin()+start,b.begin()+p); return v;
    }
    require(n<=b.size(),"metadata length outside binary");
    if(str) {range(b,p,n); v.kind=Value::String;v.s.assign(reinterpret_cast<const char*>(b.data()+p),n);p+=n;}
    else if(array) {v.kind=Value::Array;for(size_t i=0;i<n;++i)v.items.push_back(decode(b,p,depth+1));}
    else if(map) {
        v.kind=Value::Map;
        for(size_t i=0;i<n;++i) {
            // AMD's proprietary compiler (the RX 6000 driver) keys .registers by register number: such a key is
            // kept as a NUL and its MessagePack bytes, which no named lookup matches and encode writes back.
            const size_t ks=p; auto k=decode(b,p,depth+1);
            std::string key=k.kind==Value::String?std::move(k.s):std::string(1,'\0')+std::string(b.begin()+ks,b.begin()+p);
            for(const auto& f:v.fields) require(f.first!=key,"duplicate metadata key");
            v.fields.emplace_back(std::move(key),decode(b,p,depth+1));
        }
    } return v;
}
inline void putbig(Bytes& b,uint64_t n,size_t bytes) { while(bytes) b.push_back(uint8_t(n>>(--bytes*8))); }
inline void encode(Bytes& b,const Value& v) {
    switch(v.kind) {
    case Value::Raw: b.insert(b.end(),v.raw.begin(),v.raw.end()); break;
    case Value::UInt: b.push_back(0xcf);putbig(b,v.u,8);break;
    case Value::String: b.push_back(0xdb);putbig(b,v.s.size(),4);b.insert(b.end(),v.s.begin(),v.s.end());break;
    case Value::Array: b.push_back(0xdd);putbig(b,v.items.size(),4);for(const auto& x:v.items)encode(b,x);break;
    case Value::Map: b.push_back(0xdf);putbig(b,v.fields.size(),4);for(const auto& f:v.fields) {
        if(!f.first.empty() && f.first[0]=='\0') b.insert(b.end(),f.first.begin()+1,f.first.end());
        else {Value k;k.kind=Value::String;k.s=f.first;encode(b,k);}
        encode(b,f.second);
    } break;
    }
}
inline Value unpack(const Bytes& b) { size_t p=0;auto v=decode(b,p);require(p==b.size(),"trailing metadata");return v; }
inline Value array(std::initializer_list<uint64_t> xs) { Value v;v.kind=Value::Array;for(auto x:xs)v.items.emplace_back(x);return v; }

// The code objects' ELF e_flags (LLVM EF_AMDGPU_MACH). gfx12: gfx1200 0x48 (RX 9060), gfx1201 0x4e (RX 9070,
// Radeon AI PRO R9700). gfx11: gfx1100 0x41 (RX 7900), gfx1101 0x46 (RX 7800/7700), gfx1102 0x47 (RX 7600),
// gfx1103 0x44 (780M); gfx11.5: gfx1150 0x43, gfx1151 0x4a, gfx1152 0x55, gfx1153 0x58. gfx10.3: gfx1030 0x36
// (RX 6800/6900), gfx1031 0x37 (RX 6700), gfx1032 0x38 (RX 6600), gfx1033 0x39, gfx1034 0x3e (RX 6400/6500),
// gfx1035 0x3d, gfx1036 0x45.
inline bool is_gfx12(uint32_t m) { return m==0x48 || m==0x4e; }
inline bool is_gfx11(uint32_t m) { return m==0x41 || m==0x46 || m==0x47 || m==0x44 || m==0x43 || m==0x4a || m==0x55 || m==0x58; }
inline bool is_gfx103(uint32_t m) { return m==0x36 || m==0x37 || m==0x38 || m==0x39 || m==0x3e || m==0x3d || m==0x45; }
// One generation's code runs on every chip of it: ACO's gfx11 code differs between chips only in how many VGPRs
// it aims to leave free (gfx1100/1101/1151 have 1.5x the register file), never in the instructions.
inline bool same_generation(uint32_t a, uint32_t b) {
    return (is_gfx12(a) && is_gfx12(b)) || (is_gfx11(a) && is_gfx11(b)) || (is_gfx103(a) && is_gfx103(b));
}
// The machine of a code object's e_flags. gfx10.3 objects from AMD's proprietary compiler (the RX 6000 driver
// branch) also carry the xnack/sramecc fields (EF_AMDGPU_FEATURE_*_V4, bits 8-11: 0x938 on an RX 6600 = gfx1032,
// xnack any, sramecc off); gfx11 and gfx12 take exact values only.
inline uint32_t elf_machine(uint32_t flags) { return is_gfx103(flags&0xff) && !(flags&~0xfffu) ? flags&0xff : flags; }
inline const char* gfx103_isa(uint32_t m) {
    switch(m) {
    case 0x36: return "amdgcn--amdpal--gfx1030"; case 0x37: return "amdgcn--amdpal--gfx1031";
    case 0x38: return "amdgcn--amdpal--gfx1032"; case 0x39: return "amdgcn--amdpal--gfx1033";
    case 0x3e: return "amdgcn--amdpal--gfx1034"; case 0x3d: return "amdgcn--amdpal--gfx1035";
    case 0x45: return "amdgcn--amdpal--gfx1036"; default: return nullptr;
    }
}

struct Record {
    uint64_t spv_hash=0,code_hash=0;
    uint32_t rsrc1=0,rsrc2=0,rsrc3=0,exec=0,wg=0,shared=0,push_bytes=0,user=0;
    int set0=-1,push=-1,grid=-1;
    // NRPAL002: the code object machine the code was made for, and the scratch it needs (bytes a lane).
    uint32_t mach=0,scratch=0;
    std::vector<std::pair<int,int>> inlines;
    Bytes code;
    static Record parse(const Bytes& b) {
        range(b,0,96);
        const bool v2=std::memcmp(b.data(),"NRPAL002",8)==0;
        require(v2 || std::memcmp(b.data(),"NRPAL001",8)==0,"bad adapter record version");
        Record r;r.spv_hash=read<uint64_t>(b,8);r.code_hash=read<uint64_t>(b,16);
        r.rsrc1=read<uint32_t>(b,24);r.rsrc2=read<uint32_t>(b,28);r.rsrc3=read<uint32_t>(b,32);
        r.exec=read<uint32_t>(b,36);r.wg=read<uint32_t>(b,40);r.shared=read<uint32_t>(b,44);
        r.push_bytes=read<uint32_t>(b,48);r.user=read<uint32_t>(b,52);
        r.set0=read<int32_t>(b,56);r.push=read<int32_t>(b,60);r.grid=read<int32_t>(b,64);
        auto n=read<uint32_t>(b,68),sz=read<uint32_t>(b,72);
        require(n<=32 && sz && sz%4==0 && uint64_t(96)+8*n+sz==b.size(),"adapter record sizes");
        if(v2) {r.mach=read<uint32_t>(b,76);r.scratch=read<uint32_t>(b,80);}
        for(size_t i=v2?84:76;i<96;++i)require(b[i]==0,"adapter reserved field");
        for(unsigned i=0;i<n;++i)r.inlines.emplace_back(read<int32_t>(b,96+8*i),read<int32_t>(b,100+8*i));
        r.code=Bytes(b.begin()+96+8*n,b.end());
        require(hash(r.code.data(),r.code.size())==r.code_hash,"adapter code checksum");
        if(v2) {
            // gfx11/gfx10.3: scratch only on gfx11 (the SPI sets the scratch base; gfx10.3 code would need a ring
            // table PAL does not have). LDS is allocated in 512-byte units there and may exceed the shader's use.
            require(is_gfx11(r.mach) || is_gfx103(r.mach),"NRPAL002 record for an unsupported machine");
            const bool scratch_en=r.rsrc2&1;
            require(!(r.rsrc2&~uint32_t(0x00ff9fbf)) && !(r.rsrc3&~uint32_t(0xff0)) && r.user==((r.rsrc2>>1)&31) &&
                    scratch_en==(r.scratch!=0) && (!scratch_en || is_gfx11(r.mach)) && r.scratch%4==0 &&
                    r.scratch<=256*1024,"unsupported scratch/config");
            require(r.exec && r.exec<=sz && r.exec%4==0 && r.wg && r.wg<=1024 && r.push_bytes<=128,"adapter resource limits");
            require(r.shared<=(((r.rsrc2>>15)&511)*512),"adapter LDS allocation mismatch");
        } else {
        // NRPAL001 (gfx12) as upstream checks it.
        require(!(r.rsrc2&~uint32_t(0x00ff9fbf)) && !(r.rsrc3&~uint32_t(0xff0)) &&
                r.user==((r.rsrc2>>1)&31),"unsupported scratch/config");
        require(r.exec && r.exec<=sz && r.exec%4==0 && r.wg && r.wg<=1024 && r.push_bytes<=128,"adapter resource limits");
        require(r.shared<=(((r.rsrc2>>15)&511)*512),"adapter LDS allocation mismatch");
        }
        std::vector<bool> used(r.user,false);
        auto take=[&](int reg) {require(reg>=0 && uint32_t(reg)<r.user && !used[reg],"overlapping adapter arguments");used[reg]=true;};
        take(r.set0);if(r.push>=0)take(r.push);
        for(auto [reg,dw]:r.inlines) {take(reg);require(dw>=0 && uint32_t(dw)*4<r.push_bytes,"push index outside block");}
        if(r.grid>=0) for(int j=0;j<3;++j)take(r.grid+j);
        require(std::all_of(used.begin(),used.end(),[](bool x){return x;}),"unhandled adapter argument");
        for(size_t p=r.exec;p<r.code.size();p+=4)require(read<uint32_t>(r.code,p)==0xbf9f0000,"constant data unsupported");
        return r;
    }
};

struct Arguments {
    std::vector<uint64_t> map=std::vector<uint64_t>(32,0xffffffff);
    std::vector<std::pair<int,int>> moves,loads;
    unsigned user=0;int spill=-1,grid=-1;
    // push_base: the user-data entry of the first push-constant dword (XglLayout below; 1 in the plain layout)
    explicit Arguments(const Record& r,unsigned descriptor_slot=33,unsigned push_base=1) {
        map[0]=0x10000000;map[1]=descriptor_slot;user=2;moves.emplace_back(r.set0,1);
        unsigned extra=(r.grid>=0?2:0)+(r.push>=0?1:0);
        bool spill_needed=r.push>=0 || user+r.inlines.size()+extra>16;
        // A spill slot replaces the push-pointer slot, or one inline slot.
        unsigned limit=16-(r.grid>=0?2:0)-(spill_needed?1:0);
        for(auto [dst,dw]:r.inlines) {
            if(user<limit) {map[user]=dw+push_base;moves.emplace_back(dst,int(user++));}
            else loads.emplace_back(dst,4*(dw+push_base));
        }
        if(spill_needed) {spill=int(user);map[user++]=0x10000002;}
        if(r.grid>=0) {grid=int(user);map[user]=0x10000006;user+=2;}
        require(user<=16,"PAL argument limit");
        for(auto [dst,src]:moves)require(dst<=src,"unsupported parallel argument copy");
    }
};
// gfx11 and gfx10.3: the workgroup IDs, the workgroup info and the scratch wave offset arrive in SGPRs right after
// the user SGPRs (in that order, each when its rsrc2 bit is set), so they follow the user SGPR count: PAL's
// (a.user) to the record's (r.user). The instructions are the same as gfx12's but for the encodings: gfx10.3's
// SOP1/SOPP opcodes differ and its m0 and null are 124 and 125 (gfx11/12: 125 and 124). The address32 high word
// is the PC's high dword as it is (gfx12 sign-extends its low half).
inline Bytes prologue_gfx11(const Record& r,unsigned descriptor_slot,uint32_t mach,unsigned push_base) {
    Bytes b;Arguments a(r,descriptor_slot,push_base);
    const bool g10=is_gfx103(mach);
    const uint32_t op_mov=g10?0x03:0x00,op_getpc=g10?0x1f:0x47,null_reg=g10?125:124,m0_reg=g10?124:125;
    auto mov=[&](unsigned dst,unsigned src){word(b,0xbe800000|(dst<<16)|(op_mov<<8)|src);};
    auto nop=[&](unsigned n){word(b,0xbf800000|n);};
    auto ld=[&](unsigned dst,unsigned base,unsigned off) {
        word(b,0xf4000000|(dst<<6)|(base>>1));word(b,(null_reg<<25)|off);
    };
    std::vector<std::pair<unsigned,unsigned>> sys;   // (record SGPR, PAL SGPR) of each system SGPR
    {
        unsigned k=0;
        for(unsigned bit:{7u,8u,9u,10u,0u}) if(r.rsrc2&(1u<<bit)) {sys.emplace_back(r.user+k,a.user+k);++k;}
    }
    if(a.spill>=0)mov(96,a.spill);
    if(a.grid>=0) {mov(100,a.grid);mov(101,a.grid+1);}
    // Upward (the record has more user SGPRs): highest first, before the argument copies, whose sources lie below.
    if(r.user>a.user) for(size_t i=sys.size();i-->0;) mov(sys[i].first,sys[i].second);
    word(b,0xbe800000|(98<<16)|(op_getpc<<8)); // s_getpc_b64 s[98:99]
    nop(2);
    mov(97,99); // s97 = PAL address32 high (the spill table pointer's high half)
    nop(2);mov(m0_reg,97); // m0 = the same, for the body
    nop(2);
    for(auto [dst,src]:a.moves)mov(dst,src);
    // Downward: lowest first, after the argument copies (their targets lie below the record's user count).
    if(r.user<a.user) for(auto [dst,src]:sys) mov(dst,src);
    for(auto [reg,offset]:a.loads)ld(reg,96,offset);
    // the push block's address: the spill table (s96) plus push_base entries (s_add_u32 with an inline constant)
    if(r.push>=0) {word(b,0x80000000|(uint32_t(r.push)<<16)|((0x80+4*push_base)<<8)|96);nop(2);}
    if(r.grid>=0)for(unsigned j=0;j<3;++j)ld(r.grid+j,100,j*4);
    word(b,g10?0xbf8cc07fu:0xbf89fc07u); // s_waitcnt lgkmcnt(0)
    nop(3);
    // Branch over alignment padding rather than executing one nop per dword.
    size_t end=(b.size()+4+255)&~size_t(255);
    word(b,(g10?0xbf820000u:0xbfa00000u)|uint32_t((end-b.size()-4)/4));
    while(b.size()<end)nop(0);
    return b;
}
// Incoming pointers are saved before argument copies; the body keeps its
// relative branch offsets and uses m0 for the 32-bit address high word.
inline Bytes prologue(const Record& r,unsigned descriptor_slot=33,uint32_t mach=0x4e,unsigned push_base=1) {
    require(push_base>=1 && 4*push_base<=64,"push base out of range");   // its byte offset an inline constant
    if(!is_gfx12(mach))return prologue_gfx11(r,descriptor_slot,mach,push_base);
    Bytes b;Arguments a(r,descriptor_slot,push_base);
    auto mov=[&](unsigned dst,unsigned src){word(b,0xbe800000|(dst<<16)|src);};
    if(a.spill>=0)mov(96,a.spill);
    if(a.grid>=0) {mov(100,a.grid);mov(101,a.grid+1);}
    if(r.rsrc2&(1<<10)) mov(102,a.user);
    word(b,0xbe804700|(98<<16)); // s_getpc_b64 s[98:99]
    word(b,0xbf800002); // dependency spacing, also safe before sext/m0
    word(b,0xbe800f00|(97<<16)|99); // s_sext_i32_i16 s97, s99
    word(b,0xbf800002);mov(125,97); // m0 = PAL address32 high
    word(b,0xbf800002);
    auto ld=[&](unsigned dst,unsigned base,unsigned off) {
        word(b,0xf4000000|(dst<<6)|(base>>1));word(b,0xf8000000|off);
    };
    for(auto [dst,src]:a.moves)mov(dst,src);
    for(auto [reg,offset]:a.loads)ld(reg,96,offset);
    // the push block's address: the spill table (s96) plus push_base entries (s_add_u32 with an inline constant)
    if(r.push>=0) {word(b,0x80000000|(uint32_t(r.push)<<16)|((0x80+4*push_base)<<8)|96);word(b,0xbf800002);}
    if(r.grid>=0)for(unsigned j=0;j<3;++j)ld(r.grid+j,100,j*4);
    word(b,0xbfc70000);word(b,0xbf800003);
    if(r.rsrc2&(1<<10))mov(r.user,102);
    // Branch over alignment padding rather than executing one nop per dword.
    size_t end=(b.size()+4+255)&~size_t(255);
    word(b,0xbfa00000|uint32_t((end-b.size()-4)/4));
    while(b.size()<end)word(b,0xbf800000);
    return b;
}

// The code objects the adapter takes, by ELF e_flags: gfx12 (NRPAL001 records, upstream's), gfx11 and gfx10.3
// (NRPAL002). MochizukiNrRuntime.cpp asks before it would take an RDNA3 network's records.
inline bool takes_machine(uint32_t mach) { return is_gfx12(mach) || is_gfx11(mach) || is_gfx103(mach); }

struct Section { uint32_t name,type;uint64_t flags,addr,offset,size;uint32_t link,info;uint64_t align,entsize; };
static_assert(sizeof(Section)==64);
struct Elf {
    uint32_t mach=0;   // e_flags' machine (elf_machine): the machine the container's code is for
    Bytes header;std::vector<Section> sections;std::vector<Bytes> data;std::vector<std::string> names;
    Elf()=default;
    explicit Elf(const Bytes& b) {
        range(b,0,64);
        mach=elf_machine(read<uint32_t>(b,48));
        require(std::memcmp(b.data(),"\x7f" "ELF",4)==0 && b[4]==2 && b[5]==1 && b[7]==65 &&
                read<uint16_t>(b,16)==1 && read<uint16_t>(b,18)==224 && takes_machine(mach) &&
                read<uint64_t>(b,32)==0 && read<uint16_t>(b,56)==0 && read<uint16_t>(b,58)==64,"unsupported PAL ELF");
        auto count=read<uint16_t>(b,60),str=read<uint16_t>(b,62); auto at=read<uint64_t>(b,40);
        require(count>0 && count<256 && str<count,"ELF section count");range(b,at,size_t(count)*64);
        header=Bytes(b.begin(),b.begin()+64);
        for(unsigned i=0;i<count;++i) {
            auto s=read<Section>(b,at+i*64);require(s.type!=4 && s.type!=9 && s.type!=8,"relocations/NOBITS unsupported");
            require(s.align<=4096 && (!s.align || (s.align&(s.align-1))==0),"ELF section alignment");
            range(b,s.offset,s.size);sections.push_back(s);data.emplace_back(b.begin()+s.offset,b.begin()+s.offset+s.size);
        }
        require(sections[str].type==3,"ELF section strings");
        for(const auto& s:sections) {
            range(data[str],s.name,1);auto it=std::find(data[str].begin()+s.name,data[str].end(),0);
            require(it!=data[str].end(),"unterminated ELF section name");names.emplace_back(data[str].begin()+s.name,it);
        }
    }
    size_t find(const std::string& name) const {
        auto it=std::find(names.begin(),names.end(),name);require(it!=names.end(),"missing ELF section");
        require(std::count(names.begin(),names.end(),name)==1,"duplicate ELF section");return size_t(it-names.begin());
    }
    Bytes build() {
        Bytes out=header;
        for(size_t i=1;i<sections.size();++i) {
            auto& s=sections[i];size_t align=std::max<uint64_t>(s.align,1);
            out.resize((out.size()+align-1)&~(align-1));s.offset=out.size();s.size=data[i].size();
            out.insert(out.end(),data[i].begin(),data[i].end());
        }
        out.resize((out.size()+7)&~size_t(7));write(out,40,uint64_t(out.size()));
        for(const auto& s:sections) {size_t p=out.size();out.resize(p+64);write(out,p,s);}return out;
    }
};
// The PAL metadata (the AMDGPU note of type 32) of a pipeline's .note section.
inline Value pal_metadata(const Bytes& src) {
    for(size_t pos=0;pos<src.size();) {
        range(src,pos,12);
        const auto ns=read<uint32_t>(src,pos),ds=read<uint32_t>(src,pos+4),type=read<uint32_t>(src,pos+8);
        const size_t name=pos+12,desc=name+((uint64_t(ns)+3)&~uint64_t(3)),next=desc+((uint64_t(ds)+3)&~uint64_t(3));
        range(src,name,next-name);
        if(type==32) return unpack(Bytes(src.begin()+desc,src.begin()+desc+ds));
        pos=next;
    }
    throw std::runtime_error("PAL metadata missing");
}
// Where XGL put the push constants and set 0's pointer in a compute pipeline's user data (compact layout,
// icd/api/vk_pipeline_layout.cpp BuildCompactSchemeInfo): entry 0 the vertex-buffer table, then entries the device's
// features reserve - one for the uber-fetch shader's constant buffer when VK_EXT_vertex_input_dynamic_state or
// VK_EXT_graphics_pipeline_library is enabled (emulators, DXVK), one for transform feedback when
// VK_EXT_transform_feedback is (DXVK), one for debug printf -, then the push constants, then set 0's pointer, the
// last entry (.user_data_limit - 1). A plain device puts the push constants at entry 1. Anything else (ray tracing's
// entries between the push constants and the set) is refused: the network then runs from the driver's compiler.
// Upstream v0.0.9 (GitLab issue 5).
struct XglLayout { unsigned push_base, descriptor_slot; };
inline XglLayout xgl_layout_of_limit(uint64_t limit,unsigned push_bytes) {
    const uint64_t dwords=push_bytes/4;
    require(limit>=dwords+2 && limit-1-dwords<=16,"unsupported XGL user-data layout");
    return XglLayout{unsigned(limit-1-dwords),unsigned(limit-1)};
}
inline XglLayout xgl_layout(const Value& meta,unsigned push_bytes) {
    const auto& ps=meta.at("amdpal.pipelines");require(ps.kind==Value::Array && ps.items.size()==1,"PAL pipeline count");
    const uint64_t dwords=push_bytes/4;
    const XglLayout l=xgl_layout_of_limit(ps.items[0].at(".user_data_limit").number(),push_bytes);
    if(l.push_base!=1) {   // entries before the push constants: the capture must show the push constants there
        const auto& map=ps.items[0].at(".hardware_stages").at(".cs").at(".user_data_reg_map");
        require(map.kind==Value::Array,"PAL user-data map");
        bool push_seen=dwords==0;
        for(const auto& v:map.items) {
            const uint64_t x=v.number();
            if(x>=0x10000000) continue;   // the global table, the spill table, the grid, unused
            require(x>=l.push_base && x<=l.descriptor_slot,"unsupported XGL user-data layout");
            push_seen=push_seen || x<l.descriptor_slot;
        }
        require(push_seen,"unsupported XGL user-data layout");
    }
    return l;
}
// The record's code in a PAL container (the driver's own, or synthesize's), its metadata set to the record's.
// layout: where the device's pipeline layout puts the push constants and set 0 (xgl_layout).
inline Bytes splice_elf(Elf& e,const Record& r,unsigned push_bytes,std::array<uint32_t,3> workgroup,XglLayout layout) {
    require(push_bytes<=128 && push_bytes%4==0 && r.push_bytes<=push_bytes,"unsupported pipeline push range");
    const unsigned descriptor_slot=layout.descriptor_slot;
    require(descriptor_slot==layout.push_base+push_bytes/4,"unsupported XGL user-data layout");
    auto text=e.find(".text"),note=e.find(".note"),sym=e.find(".symtab");
    // NRPAL001 records are gfx12 code (upstream's); NRPAL002 name their machine.
    require(r.mach ? same_generation(r.mach,e.mach) : is_gfx12(e.mach),"record and GPU are different generations");
    require(e.sections[text].align==256 && e.sections[text].addr==0,"PAL entry alignment");
    // Exactly one compute entry, no relocations, no secondary functions/data.
    const auto& sy=e.sections[sym];require(sy.entsize==24 && sy.link<e.data.size() && e.data[sym].size()%24==0,"ELF symbols");
    unsigned found=0;
    for(size_t p=0;p<e.data[sym].size();p+=24) {
        auto ix=read<uint16_t>(e.data[sym],p+6);if(ix!=text)continue;
        auto off=read<uint32_t>(e.data[sym],p);range(e.data[sy.link],off,1);
        auto end=std::find(e.data[sy.link].begin()+off,e.data[sy.link].end(),0);
        require(end!=e.data[sy.link].end(),"unterminated symbol");
        std::string name(e.data[sy.link].begin()+off,end);
        require(name=="_amdgpu_cs_main" && (e.data[sym][p+4]&15)==2 && read<uint64_t>(e.data[sym],p+8)==0,
                "unsupported PAL text symbol");
        auto pre=prologue(r,descriptor_slot,e.mach,layout.push_base);write(e.data[sym],p+16,uint64_t(pre.size()+r.exec));++found;
    }
    require(found==1,"PAL compute entry count");
    auto pre=prologue(r,descriptor_slot,e.mach,layout.push_base);e.data[text]=pre;e.data[text].insert(e.data[text].end(),r.code.begin(),r.code.end());
    Bytes notes;size_t pos=0;unsigned metadata=0;
    while(pos<e.data[note].size()) {
        auto& src=e.data[note];size_t start=pos;range(src,pos,12);
        auto ns=read<uint32_t>(src,pos),ds=read<uint32_t>(src,pos+4),type=read<uint32_t>(src,pos+8);
        pos+=12;range(src,pos,(uint64_t(ns)+3)&~uint64_t(3));
        Bytes name(src.begin()+pos,src.begin()+pos+ns);pos+=(uint64_t(ns)+3)&~uint64_t(3);
        range(src,pos,(uint64_t(ds)+3)&~uint64_t(3));size_t desc=pos;pos+=(uint64_t(ds)+3)&~uint64_t(3);
        if(type!=32) {notes.insert(notes.end(),src.begin()+start,src.begin()+pos);continue;}
        require(name==Bytes({'A','M','D','G','P','U',0}),"unexpected PAL note owner");
        auto m=unpack(Bytes(src.begin()+desc,src.begin()+desc+ds));
        auto& ver=m.at("amdpal.version");require(ver.kind==Value::Array && ver.items.size()==2 &&
                ver.items[0].number()==3 && ver.items[1].number()==6,"PAL metadata version");
        auto& ps=m.at("amdpal.pipelines");require(ps.kind==Value::Array && ps.items.size()==1,"PAL pipeline count");
        auto& p=ps.items[0];require(p.at(".user_data_limit").number()==descriptor_slot+1 && p.at(".api").s=="Vulkan" &&
                p.at(".type").s=="Cs","unsupported XGL user-data layout");
        auto& cs=p.at(".hardware_stages").at(".cs");
        require(cs.at(".wavefront_size").number()==32 && cs.at(".entry_point_symbol").s=="_amdgpu_cs_main","PAL compute ABI");
        const auto& dims=cs.at(".threadgroup_dimensions");
        require(dims.kind==Value::Array && dims.items.size()==3,"invalid PAL workgroup dimensions");
        if(workgroup[0]) {
            require(workgroup[1] && workgroup[2] && uint64_t(workgroup[0])*workgroup[1]*workgroup[2]==r.wg,
                    "workgroup size differs from capture");
            cs.set(".threadgroup_dimensions",array({workgroup[0],workgroup[1],workgroup[2]}));
        } else require(dims.items[0].number()*dims.items[1].number()*dims.items[2].number()==r.wg,
                       "workgroup size differs from capture");
        auto& map=cs.at(".user_data_reg_map");require(map.kind==Value::Array && map.items.size()==32 &&
                map.items[0].number()==0x10000000,"PAL global table ABI");
        for(const auto& v:map.items) {auto x=v.number();require(x<=descriptor_slot || x==0xffffffff || x==0x10000000 || x==0x10000002 || x==0x10000006,"unhandled PAL user-data mapping");}
        Arguments a(r,descriptor_slot,layout.push_base);map.items.clear();for(auto x:a.map)map.items.emplace_back(x);
        p.set(".spill_threshold",Value(a.spill>=0?1:65535));
        cs.set(".shader_spill_threshold",Value(a.spill>=0?1:65535));
        cs.set(".user_sgprs",Value(a.user));
        cs.set(".sgpr_count",Value(103));
        cs.set(".vgpr_count",Value(((r.rsrc1&63)+1)*8));
        cs.set(".lds_size",Value(((r.rsrc2>>15)&511)*512));cs.set(".float_mode",Value((r.rsrc1>>12)&255));
        cs.set(".wgp_mode",Value::boolean((r.rsrc1>>29)&1));
        cs.set(".mem_ordered",Value::boolean((r.rsrc1>>30)&1));
        cs.set(".forward_progress",Value::boolean((r.rsrc1>>31)&1));
        cs.set(".fp16_overflow",Value::boolean((r.rsrc1>>26)&1));
        cs.set(".scratch_en",Value::boolean(r.scratch!=0));cs.set(".scratch_memory_size",Value(r.scratch));
        auto& cr=p.at(".compute_registers");
        for(unsigned i=0;i<3;++i)cr.set(i==0?".tgid_x_en":i==1?".tgid_y_en":".tgid_z_en",Value::boolean((r.rsrc2>>(7+i))&1));
        cr.set(".tg_size_en",Value::boolean((r.rsrc2>>10)&1));cr.set(".tidig_comp_cnt",Value((r.rsrc2>>11)&3));
        cr.set(".x_interleave",Value(0));cr.set(".y_interleave",Value(0));
        auto h=hash(e.data[text].data(),e.data[text].size());
        cs.set(".checksum_value",Value(uint32_t(h)));p.set(".internal_pipeline_hash",array({h,r.spv_hash}));
        Bytes md;encode(md,m);word(notes,ns);word(notes,uint32_t(md.size()));word(notes,type);
        notes.insert(notes.end(),name.begin(),name.end());while(notes.size()%4)notes.push_back(0);
        notes.insert(notes.end(),md.begin(),md.end());while(notes.size()%4)notes.push_back(0);++metadata;
    }
    require(metadata==1,"PAL metadata note count");e.data[note]=std::move(notes);return e.build();
}
inline Bytes splice(const Bytes& binary,const Record& r,unsigned push_bytes=128,
                    std::array<uint32_t,3> workgroup={0,0,0}) {
    Elf e(binary);
    require(push_bytes<=128 && push_bytes%4==0,"unsupported pipeline push range");
    const XglLayout layout=xgl_layout(pal_metadata(e.data[e.find(".note")]),push_bytes);
    return splice_elf(e,r,push_bytes,workgroup,layout);
}

// gfx10.3 (RX 6000): the driver's own code objects come from AMD's proprietary compiler, with metadata of its own
// (registers keyed by number, no global table entry, other keys), and are not spliced. What that driver (26.8.1 on
// an RX 6600, probe 2) imports bit-exact is the LLPC container instead: the RX 9070 XT's layout and metadata keys,
// with the GPU's machine in e_flags (no feature bits) and EI_ABIVERSION 0 (1-3 are refused). It is built here, so a
// gfx10.3 pipeline needs no compile of its template. mach: the GPU's machine, from a code object the driver made.
// push_base: where the device's layout puts the push constants (device_code reads it from the driver's empty kernel).
inline Bytes synthesize(const Record& r,uint32_t mach,unsigned push_bytes,std::array<uint32_t,3> workgroup,
                        unsigned push_base=1) {
    const char* isa=gfx103_isa(mach);
    require(isa && r.mach && is_gfx103(r.mach),"record and GPU are different generations");
    require(workgroup[0] && workgroup[1] && workgroup[2],"literal SPIR-V local size required");
    // IEEE mode is not in this container's metadata; ACO's gfx10.3 compute code never sets it.
    require(!((r.rsrc1>>23)&1),"IEEE mode unsupported on gfx10.3");
    require(push_bytes<=128 && push_bytes%4==0,"unsupported pipeline push range");
    const XglLayout layout=xgl_layout_of_limit(push_base+push_bytes/4+1,push_bytes);
    const unsigned slot=layout.descriptor_slot;
    Elf e;e.mach=mach;e.header=Bytes(64,0);
    std::memcpy(e.header.data(),"\x7f" "ELF\x02\x01\x01\x41",8);
    write(e.header,16,uint16_t(1));write(e.header,18,uint16_t(224));write(e.header,20,uint32_t(1));
    write(e.header,48,mach);write(e.header,52,uint16_t(64));write(e.header,58,uint16_t(64));
    const std::string strtab=std::string("\0.text\0.AMDGPU.gpr_maximums\0_amdgpu_cs_main\0.note.GNU-stack\0.note\0.strtab\0.symtab\0",82);
    auto at=[&](const char* n){return uint32_t(strtab.find(std::string(n)+'\0',1));};
    auto add=[&](const char* n,uint32_t type,uint64_t flags,uint64_t align,Bytes d,uint32_t link=0,uint32_t info=0,uint64_t ent=0) {
        Section s{};s.name=*n?at(n):0;s.type=type;s.flags=flags;s.align=align;s.link=link;s.info=info;s.entsize=ent;
        e.sections.push_back(s);e.data.push_back(std::move(d));e.names.emplace_back(n);
    };
    add("",0,0,0,{});
    add(".strtab",3,0,1,Bytes(strtab.begin(),strtab.end()));
    add(".text",1,6,256,{});
    add(".AMDGPU.gpr_maximums",1,0,1,{});
    add(".note.GNU-stack",1,0,1,{});
    // Notes: the ISA name (owner AMD, type 11), then the PAL metadata (owner AMDGPU, type 32).
    Bytes notes;word(notes,4);word(notes,uint32_t(std::strlen(isa)));word(notes,11);
    notes.insert(notes.end(),{'A','M','D',0});notes.insert(notes.end(),isa,isa+std::strlen(isa));
    while(notes.size()%4)notes.push_back(0);
    {
        auto map=[](std::vector<std::pair<std::string,Value>> f){Value v;v.kind=Value::Map;v.fields=std::move(f);return v;};
        auto str=[](const char* s){Value v;v.kind=Value::String;v.s=s;return v;};
        auto f=Value::boolean(false);
        Value reg;reg.kind=Value::Array;reg.items.emplace_back(uint64_t(0x10000000));
        for(int i=1;i<32;++i)reg.items.emplace_back(uint64_t(0xffffffff));
        Value hw_map;hw_map.kind=Value::Array;hw_map.items.push_back(str(".cs"));
        Value cs=map({{".checksum_value",Value(0)},{".debug_mode",f},{".entry_point_symbol",str("_amdgpu_cs_main")},
            {".excp_en",Value(0)},{".float_mode",Value(0)},{".forward_progress",f},{".image_op",f},{".lds_size",Value(0)},
            {".mem_ordered",f},{".scratch_en",f},{".scratch_memory_size",Value(0)},{".sgpr_count",Value(0)},
            {".sgpr_limit",Value(106)},{".threadgroup_dimensions",array({workgroup[0],workgroup[1],workgroup[2]})},
            {".trap_present",f},{".user_data_reg_map",reg},{".user_sgprs",Value(0)},{".vgpr_count",Value(0)},
            {".vgpr_limit",Value(256)},{".wavefront_size",Value(32)},{".wg_round_robin",f},{".wgp_mode",f}});
        Value p=map({{".api",str("Vulkan")},
            {".compute_registers",map({{".tg_size_en",f},{".tgid_x_en",f},{".tgid_y_en",f},{".tgid_z_en",f},
                                       {".tidig_comp_cnt",Value(0)},{".x_interleave",Value(0)},{".y_interleave",Value(0)}})},
            {".hardware_stages",map({{".cs",cs}})},{".internal_pipeline_hash",array({0,0})},
            {".shaders",map({{".compute",map({{".api_shader_hash",array({r.spv_hash,0})},{".hardware_mapping",hw_map}})}})},
            {".spill_threshold",Value(65535)},{".type",str("Cs")},{".user_data_limit",Value(slot+1)},
            {".xgl_cache_info",map({{".128_bit_cache_hash",array({r.spv_hash,r.code_hash})},{".llpc_version",str("89.0")}})}});
        Value ps;ps.kind=Value::Array;ps.items.push_back(std::move(p));
        Bytes md;encode(md,map({{"amdpal.pipelines",std::move(ps)},{"amdpal.version",array({3,6})}}));
        word(notes,7);word(notes,uint32_t(md.size()));word(notes,32);
        notes.insert(notes.end(),{'A','M','D','G','P','U',0,0});notes.insert(notes.end(),md.begin(),md.end());
        while(notes.size()%4)notes.push_back(0);
    }
    add(".note",7,0,4,std::move(notes));
    // The null symbol and _amdgpu_cs_main (FUNC GLOBAL in .text at 0; splice_elf sets its size).
    Bytes sym(48,0);write(sym,24,at("_amdgpu_cs_main"));sym[28]=0x12;write(sym,30,uint16_t(2));
    add(".symtab",2,0,8,std::move(sym),1,1,24);
    write(e.header,60,uint16_t(e.sections.size()));write(e.header,62,uint16_t(1));
    return splice_elf(e,r,push_bytes,workgroup,layout);
}
} // namespace nr::pal
