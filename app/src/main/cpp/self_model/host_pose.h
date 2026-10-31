#pragma once
#include "self_model/model_fit.h"

namespace ft::self_model {
inline bool validHostMatrix(const Matrix& value) {
    for(float f:value.m) if(!std::isfinite(f)) return false;
    const auto m=matrix(value);
    if(std::abs(m[0].w)>1e-4f || std::abs(m[1].w)>1e-4f ||
       std::abs(m[2].w)>1e-4f || std::abs(m[3].w-1)>1e-4f) return false;
    try { (void)rotation(m); } catch(const std::exception&) { return false; }
    return true;
}


inline const char* hostPoseError(const Model& model,
        const std::vector<std::optional<Matrix>>& rest,
        const std::vector<std::optional<Matrix>>& now) {
    if(rest.size()!=model.bones.size() || now.size()!=rest.size()) return "Incomplete pose arrays";
    for(size_t i=0;i<now.size();++i) {
        if(rest[i] && !now[i]) return "Missing mapped bone";
        if(now[i] && !validHostMatrix(*now[i])) return "Invalid bone matrix";
    }
    const auto reference=humanPositions(model,&rest),current=humanPositions(model,&now);


    for(auto edge:{std::pair{1,3},std::pair{3,5},std::pair{2,4},std::pair{4,6},
                  std::pair{13,15},std::pair{15,17},std::pair{14,16},std::pair{16,18},
                  std::pair{0,7},std::pair{7,8},std::pair{8,9},std::pair{9,10}}) {
        const auto [a,b]=edge;
        if(!reference[a] || !reference[b] || !current[a] || !current[b]) continue;
        const float length=glm::length(*reference[b]-*reference[a]);
        const float live=glm::length(*current[b]-*current[a]);
        if(length>1e-4f && (!std::isfinite(live) || live<length*.1f || live>length*4.f))
            return "Distorted host bone chain";
    }
    return nullptr;
}
}
