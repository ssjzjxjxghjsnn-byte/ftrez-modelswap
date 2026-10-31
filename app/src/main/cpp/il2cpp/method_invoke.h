#pragma once
#include <span>

namespace ft::il2cpp {
enum class InvokeError {
    None,Unavailable,InvalidMethod,ApiUnavailable,ThreadNotAttached,
    ArgumentCountMismatch,NullInstance,ManagedException
};
struct InvokeResult {
    InvokeError error=InvokeError::Unavailable;
    void* value=nullptr;
    void* exception=nullptr;
    bool succeeded() const { return error==InvokeError::None; }
};
// The game's own invoker chooses AOT or HybridCLR. No IL or interpreter scan.
InvokeResult Invoke(const void* method,void* instance=nullptr,std::span<void*> args={});
}
