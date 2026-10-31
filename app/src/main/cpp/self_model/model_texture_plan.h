#pragma once
#include "self_model/model_format.h"
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace ft::self_model {



inline std::vector<int32_t> modelTextureSources(const std::vector<Material>& materials) {
    if(materials.size()>size_t(std::numeric_limits<int32_t>::max()))
        throw std::runtime_error("Too many model materials");
    std::vector<int32_t> sources(materials.size(),-1);
    std::unordered_map<std::string_view,int32_t> unique;
    for(size_t i=0;i<materials.size();++i) {
        const auto& bytes=materials[i].png;
        if(bytes.empty()) continue;
        const std::string_view key(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        sources[i]=unique.emplace(key,int32_t(i)).first->second;
    }
    return sources;
}
inline std::vector<int32_t> modelTextureSources(const Model& model) {
    std::vector<int32_t> result(modelTextureCount(model),-1);
    for(size_t i=0;i<result.size();++i) {
        const auto& bytes=modelTextureBytes(model,i);
        if(bytes.empty()) continue;
        result[i]=int32_t(i);
        for(size_t j=0;j<i;++j) if(result[j]==int32_t(j) &&
            modelTextureSampling(model,i)==modelTextureSampling(model,j) && bytes==modelTextureBytes(model,j)) {
            result[i]=int32_t(j); break;
        }
    }
    return result;
}
}
