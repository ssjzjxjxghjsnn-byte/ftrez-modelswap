#include "il2cpp/method_invoke.h"
#include "il2cpp/il2cpp_api.h"
#include <cstdint>

namespace ft::il2cpp {
InvokeResult Invoke(const void* method,void* instance,std::span<void*> args) {
    if(!method) return {InvokeError::InvalidMethod};
    // Resolution completes before any model or settings callback is installed.
    static auto invoke=reinterpret_cast<void*(*)(const void*,void*,void**,void**)>(lookup("il2cpp_runtime_invoke"));
    static auto current=reinterpret_cast<void*(*)()>(lookup("il2cpp_thread_current"));
    static auto parameters=reinterpret_cast<uint32_t(*)(const void*)>(lookup("il2cpp_method_get_param_count"));
    static auto flags=reinterpret_cast<uint32_t(*)(const void*,uint32_t*)>(lookup("il2cpp_method_get_flags"));
    if(!invoke || !current || !parameters || !flags) return {InvokeError::ApiUnavailable};
    if(!current()) return {InvokeError::ThreadNotAttached};
    if(args.size()!=parameters(method)) return {InvokeError::ArgumentCountMismatch};
    if(!(flags(method,nullptr)&0x10) && !instance) return {InvokeError::NullInstance};
    void* exception=nullptr;
    auto value=invoke(method,instance,args.empty()?nullptr:args.data(),&exception);
    return exception?InvokeResult{InvokeError::ManagedException,nullptr,exception}:
                     InvokeResult{InvokeError::None,value,nullptr};
}
}
