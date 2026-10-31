#pragma once
#include "self_model/model_format.h"
#include <optional>

namespace ft::self_model {
struct PlayerView { bool firstPerson=false, showArms=true, visible=true; };
inline bool armBone(const Model& model,size_t index) {
    for(size_t depth=0;index<model.bones.size() && depth<model.bones.size();++depth) {
        const auto& b=model.bones[index];
        if((b.human>=13 && b.human<=18) || (b.human>=24 && b.human<=53)) return true;
        if(b.parent<0) break;
        index=size_t(b.parent);
    }
    return false;
}
inline bool armVertex(const Model& model,const Vertex& vertex) {
    float arms=0,total=0;
    for(size_t i=0;i<vertex.weights.size();++i) {
        total+=vertex.weights[i];
        if(armBone(model,vertex.bones[i])) arms+=vertex.weights[i];
    }
    return total>0 && arms>=total*.5f;
}
inline std::vector<uint32_t> armTriangles(const Model& model,const Part& part) {
    std::vector<uint32_t> result;
    for(size_t i=part.firstIndex;i<part.firstIndex+part.indexCount;i+=3) {
        bool arm=true;
        for(size_t j=0;j<3;++j) arm &= armVertex(model,model.vertices[part.firstVertex+model.indices[i+j]]);
        if(arm) result.insert(result.end(),model.indices.begin()+i,model.indices.begin()+i+3);
    }
    return result;
}
inline bool modelVisible(bool hostActive,bool anyOriginal,std::optional<PlayerView> view) {
    if(!hostActive) return false;
    if(view) return view->visible && (view->firstPerson?view->showArms:true);
    return anyOriginal;
}
}
