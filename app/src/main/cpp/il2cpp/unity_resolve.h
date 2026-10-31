#pragma once
#include "il2cpp/il2cpp_api.h"

// Small compatibility adapter for the extracted model engine.
namespace unity {
inline void* resolve_api(const char* name) { return ft::il2cpp::lookup(name); }
}
