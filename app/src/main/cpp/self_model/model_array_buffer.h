#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ft::self_model {

template<class T,class Api> void* writeModelArray(Api& api,void* array,const T* values,size_t count) {
    static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>,"Upload elements must be plain values");
    if(!array || api.array_length(array)!=count ||
        api.array_element_size(api.object_get_class(array))!=sizeof(T) || (count && !values) ||
        count>std::numeric_limits<size_t>::max()/sizeof(T))
        throw std::runtime_error("Unity reusable array ABI mismatch");
    auto destination=reinterpret_cast<uint8_t*>(array)+api.array_object_header_size();
    if(count) std::memcpy(destination,values,count*sizeof(T));
    return array;
}
}
