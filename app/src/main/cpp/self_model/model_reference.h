#pragma once
#include <stdexcept>

namespace ft::self_model {
template<class Set,class Get>
void writeModelReference(Set set,Get get,void* object,void* field,void* value) {

    set(object,field,value);
    void* actual=nullptr;
    get(object,field,&actual);
    if(actual!=value) {
        set(object,field,nullptr);
        throw std::runtime_error("Probe anchor write verification failed");
    }
}
}
