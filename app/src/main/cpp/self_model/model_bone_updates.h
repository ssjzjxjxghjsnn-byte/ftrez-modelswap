#pragma once
#include "self_model/model_pose.h"
#include <cstring>

namespace ft::self_model {


class ModelBoneUpdates {
    struct Entry { size_t bone; PoseBone last{}; bool valid=false; };
    std::vector<Entry> entries_;
    size_t bones_=0;
public:
    void clear() { entries_.clear(); bones_=0; }
    template<class GpuPart> void prepare(const Model& model,GpuPart gpuPart) {
        clear(); bones_=model.bones.size();
        std::vector<bool> used(bones_);
        for(size_t i=0;i<model.parts.size();++i) if(gpuPart(i)) {
            const auto& part=model.parts[i];
            for(size_t j=part.firstVertex;j<part.firstVertex+part.vertexCount;++j) {
                const auto& vertex=model.vertices.at(j);
                for(size_t k=0;k<4;++k) if(vertex.weights[k]) used.at(vertex.bones[k])=true;
            }
        }
        for(size_t i=0;i<used.size();++i) if(used[i]) entries_.push_back({i});
    }
    size_t size() const { return entries_.size(); }
    template<class Write> size_t apply(const std::vector<PoseBone>& pose,Write write) {
        if(pose.size()!=bones_) throw std::runtime_error("Model bone update pose size changed");
        size_t count=0;
        for(auto& entry:entries_) {
            const auto& current=pose[entry.bone];
            if(entry.valid && !std::memcmp(&current.position,&entry.last.position,sizeof(Vec3)) &&
               !std::memcmp(&current.rotation,&entry.last.rotation,sizeof(Quat))) continue;
            write(entry.bone,current);


            entry.last=current; entry.valid=true; ++count;
        }
        return count;
    }
};
}
