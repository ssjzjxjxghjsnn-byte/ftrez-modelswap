#pragma once
#include "self_model/bone_mapping.h"
#include <map>

namespace ft::self_model {



struct HostBoneCandidate {
    uintptr_t object=0;
    Matrix bind;
    int priority=0;
    bool ambiguous=false;
};
inline int boneNamePriority(const std::string& raw) {
    std::string name;
    for(unsigned char c:raw) if(c<128 && std::isalnum(c)) name+=char(std::tolower(c));
    if(name=="bnhead") return 1;
    return 2;
}
class HostSkeleton {
    std::map<std::string,HostBoneCandidate> bones_;
public:
    void offer(const std::string& key,uintptr_t object,const Matrix& bind,int priority) {
        if(key.empty() || !object) return;
        auto& old=bones_[key];
        if(!old.object || priority>old.priority) old={object,bind,priority,false};
        else if(priority==old.priority && old.object!=object) old.ambiguous=true;
    }
    const HostBoneCandidate* get(const std::string& key) const {
        auto it=bones_.find(key);
        return it==bones_.end() || it->second.ambiguous ? nullptr : &it->second;
    }
};
}
