#pragma once
#include <cstddef>
#include <cstdint>

namespace ft::il2cpp {
struct Status {
    bool moduleLoaded=false;
    std::size_t resolved=0,total=0;
};
Status resolve();
Status status();
void* lookup(const char* name);
uintptr_t moduleBase();
// Check the loaded ELF segments (also visible with ARM64 translation) and
// process mappings. Metadata returned by IL2CPP may live on the native heap.
bool readable(uintptr_t address,std::size_t bytes);
bool executable(uintptr_t address,std::size_t bytes=4);
bool readMemory(uintptr_t address,void* destination,std::size_t bytes);
// IL2CPP's MethodInfo starts with its native method pointer. No private layout.
void* methodPointer(const void* method);
}
