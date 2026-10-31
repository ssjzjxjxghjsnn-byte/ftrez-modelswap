#pragma once
#include <cstdint>

namespace ft::self_model {
// Binding is committed before the local handle changes. A failed clone/bind
// leaves the shared source intact; the caller owns cleanup of a new allocation.
template<class Clone,class Bind>
void makeModelMeshWritable(uint32_t& mesh,bool& shared,Clone clone,Bind bind) {
    if(!shared) return;
    const auto local=clone(mesh);
    bind(local);
    mesh=local; shared=false;
}
}
