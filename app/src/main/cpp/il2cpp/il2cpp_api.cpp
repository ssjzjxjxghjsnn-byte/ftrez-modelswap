#include "il2cpp/il2cpp_api.h"
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ft::il2cpp {
namespace {
struct Range { uintptr_t begin=0,end=0; };
bool contains(const std::vector<Range>& ranges,uintptr_t address,size_t bytes) {
    if(!address || !bytes || bytes>UINTPTR_MAX-address) return false;
    for(const auto& r:ranges)
        if(address>=r.begin && address<r.end && bytes<=r.end-address) return true;
    return false;
}
struct Symbol { std::string name; uintptr_t address=0; };
struct Module {
    uintptr_t base=0,dynamic=0;
    size_t dynamicSize=0;
    std::string path;
    std::vector<Range> readable,executable;
    std::vector<Symbol> symbols;
    bool validTable=false;
    bool contains(uintptr_t p,size_t bytes) const { return il2cpp::contains(readable,p,bytes); }
    uintptr_t pointer(uintptr_t value) const {
        if(contains(value,1)) return value;
        if(value<=UINTPTR_MAX-base && contains(base+value,1)) return base+value;
        return 0;
    }
};
std::mutex mutex;
Module module;
std::unordered_map<std::string,void*> addresses;
void* handle=nullptr;
Status current{};
std::string lastMessage;

void report(const std::string& message) {
    if(lastMessage==message) return;
    lastMessage=message;
    __android_log_print(ANDROID_LOG_INFO,"FTREZ-ModelSwap","%s",message.c_str());
}

// Read only the ELF dynamic symbol table of the module already loaded by the
// game. This is ordinary exported-API resolution; no IL or code-pattern scan.
bool enumerate(Module& m) {
    if(!m.dynamic || !m.contains(m.dynamic,m.dynamicSize)) return false;
    uintptr_t symbols=0,strings=0,hash=0,gnu=0;
    size_t stringSize=0,symbolSize=0;
    bool terminated=false;
    for(size_t i=0;i<m.dynamicSize/sizeof(ElfW(Dyn));++i) {
        ElfW(Dyn) d;
        std::memcpy(&d,reinterpret_cast<void*>(m.dynamic+i*sizeof(d)),sizeof(d));
        if(d.d_tag==DT_NULL) { terminated=true; break; }
        switch(d.d_tag) {
            case DT_SYMTAB:symbols=m.pointer(d.d_un.d_ptr);break;
            case DT_STRTAB:strings=m.pointer(d.d_un.d_ptr);break;
            case DT_HASH:hash=m.pointer(d.d_un.d_ptr);break;
            case DT_GNU_HASH:gnu=m.pointer(d.d_un.d_ptr);break;
            case DT_STRSZ:stringSize=d.d_un.d_val;break;
            case DT_SYMENT:symbolSize=d.d_un.d_val;break;
        }
    }
    if(!terminated || !symbols || !strings || symbolSize!=sizeof(ElfW(Sym)) ||
       !stringSize || !m.contains(strings,stringSize)) return false;
    constexpr size_t limit=1000000;
    auto word=[&](uintptr_t p,uint32_t& value) {
        if(!m.contains(p,sizeof(value))) return false;
        std::memcpy(&value,reinterpret_cast<void*>(p),sizeof(value));return true;
    };
    uint32_t count=0;
    if(hash) {
        if(!m.contains(hash,8) || !word(hash+4,count)) return false;
    } else if(gnu) {
        uint32_t buckets=0,first=0,bloom=0;
        if(!m.contains(gnu,16) || !word(gnu,buckets) || !word(gnu+4,first) ||
           !word(gnu+8,bloom) || buckets>limit || bloom>limit || first>limit) return false;
        const size_t header=16+size_t(bloom)*sizeof(ElfW(Addr))+size_t(buckets)*4;
        if(!m.contains(gnu,header)) return false;
        const auto bucketStart=gnu+16+size_t(bloom)*sizeof(ElfW(Addr));
        const auto chains=gnu+header;
        count=first;
        size_t budget=limit;
        for(size_t i=0;i<buckets;++i) {
            uint32_t index=0;
            if(!word(bucketStart+i*4,index)) return false;
            if(!index) continue;
            if(index<first || index>=limit) return false;
            for(;;) {
                uint32_t value=0;
                if(!budget-- || index>=limit ||
                   !m.contains(chains,(size_t(index-first)+1)*4) ||
                   !word(chains+size_t(index-first)*4,value)) return false;
                ++index;
                if(index>count) count=index;
                if(value&1) break;
            }
        }
    } else return false;
    if(!count || count>limit || !m.contains(symbols,size_t(count)*symbolSize)) return false;
    for(size_t i=0;i<count;++i) {
        ElfW(Sym) s;
        std::memcpy(&s,reinterpret_cast<void*>(symbols+i*symbolSize),sizeof(s));
        if(s.st_shndx==SHN_UNDEF || s.st_name>=stringSize) continue;
        const auto binding=ELF64_ST_BIND(s.st_info),kind=ELF64_ST_TYPE(s.st_info);
        if((binding!=STB_GLOBAL && binding!=STB_WEAK) || kind!=STT_FUNC) continue;
        const char* name=reinterpret_cast<const char*>(strings+s.st_name);
        const size_t available=stringSize-s.st_name;
        if(available<8 || std::memcmp(name,"il2cpp_",7)) continue;
        const auto end=static_cast<const char*>(std::memchr(name,0,available));
        if(!end) return false;
        uintptr_t address=0;
        if(s.st_shndx==SHN_ABS) address=s.st_value;
        else if(s.st_value<=UINTPTR_MAX-m.base) address=m.base+s.st_value;
        if(!contains(m.executable,address,4) || (address&3)) continue;
        m.symbols.push_back({std::string(name,end),address});
    }
    return !m.symbols.empty();
}
int findModule(dl_phdr_info* info,size_t,void* data) {
    if(!info->dlpi_name) return 0;
    const char* name=std::strrchr(info->dlpi_name,'/');
    name=name?name+1:info->dlpi_name;
    if(std::strcmp(name,"libil2cpp.so")) return 0;
    auto& m=*static_cast<Module*>(data);
    m.base=info->dlpi_addr;m.path=info->dlpi_name;
    for(size_t i=0;i<info->dlpi_phnum;++i) {
        const auto& p=info->dlpi_phdr[i];
        if(p.p_vaddr>UINTPTR_MAX-m.base) continue;
        const auto start=m.base+p.p_vaddr;
        if(p.p_memsz>UINTPTR_MAX-start) continue;
        if(p.p_type==PT_LOAD && (p.p_flags&PF_R)) m.readable.push_back({start,start+p.p_memsz});
        if(p.p_type==PT_LOAD && (p.p_flags&PF_X)) m.executable.push_back({start,start+p.p_memsz});
        if(p.p_type==PT_DYNAMIC) { m.dynamic=start;m.dynamicSize=p.p_memsz; }
    }
    m.validTable=enumerate(m);
    return 1;
}
bool mapped(uintptr_t address,size_t bytes,bool execute) {
    if(!address || !bytes || bytes>UINTPTR_MAX-address) return false;
    FILE* maps=std::fopen("/proc/self/maps","r");
    if(!maps) return false;
    char line[1024];bool found=false;
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long start=0,end=0;char permissions[5]{};
        if(std::sscanf(line,"%lx-%lx %4s",&start,&end,permissions)!=3) continue;
        if(permissions[0]!='r' || (execute && permissions[2]!='x')) continue;
        if(address>=start && address<end && bytes<=end-address) { found=true;break; }
    }
    std::fclose(maps);return found;
}
}

Status resolve() {
    std::lock_guard lock(mutex);
    // API pointers are published once and remain stable after hooks start.
    if(current.resolved) return current;
    Module next;
    dl_iterate_phdr(findModule,&next);
    current={};current.moduleLoaded=!next.path.empty();
    if(!current.moduleLoaded) { report("Waiting for loaded libil2cpp.so");return current; }
    module=std::move(next);
    if(!handle) {
        handle=dlopen(module.path.c_str(),RTLD_NOW|RTLD_NOLOAD);
        if(!handle) handle=dlopen("libil2cpp.so",RTLD_NOW|RTLD_NOLOAD);
    }
    if(!module.validTable) {
        report("Loaded libil2cpp symbol table is unavailable; API resolution deferred");return current;
    }
    size_t fallback=0;
    for(const auto& symbol:module.symbols) {
        void* address=handle?dlsym(handle,symbol.name.c_str()):nullptr;
        if(!address) { address=reinterpret_cast<void*>(symbol.address);++fallback; }
        const auto [entry,inserted]=addresses.emplace(symbol.name,address);
        if(!inserted && entry->second!=address) entry->second=nullptr;
    }
    current.total=addresses.size();
    for(const auto& item:addresses) if(item.second) ++current.resolved;
    report("IL2CPP APIs resolved: "+std::to_string(current.resolved)+"/"+
        std::to_string(current.total)+"; ELF export fallback="+std::to_string(fallback));
    return current;
}
Status status() { std::lock_guard lock(mutex);return current; }
void* lookup(const char* name) {
    if(!name) return nullptr;
    std::lock_guard lock(mutex);
    const auto found=addresses.find(name);
    return found==addresses.end()?nullptr:found->second;
}
uintptr_t moduleBase() { std::lock_guard lock(mutex);return module.base; }
bool readable(uintptr_t address,size_t bytes) {
    { std::lock_guard lock(mutex);if(contains(module.readable,address,bytes)) return true; }
    return mapped(address,bytes,false);
}
bool executable(uintptr_t address,size_t bytes) {
    { std::lock_guard lock(mutex);if(contains(module.executable,address,bytes)) return true; }
    return mapped(address,bytes,true);
}
bool readMemory(uintptr_t address,void* destination,size_t bytes) {
    if(!destination || !readable(address,bytes)) return false;
    std::memcpy(destination,reinterpret_cast<void*>(address),bytes);return true;
}
void* methodPointer(const void* method) {
    void* pointer=nullptr;
    if(!readMemory(reinterpret_cast<uintptr_t>(method),&pointer,sizeof(pointer))) return nullptr;
    const auto address=reinterpret_cast<uintptr_t>(pointer);
    return !(address&3) && executable(address)?pointer:nullptr;
}
}
