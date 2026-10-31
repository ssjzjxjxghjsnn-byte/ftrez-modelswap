#pragma once
#include "self_model/model_pose.h"
#include "self_model/bone_mapping.h"
#include <limits>

namespace ft::self_model {
using HumanPositions=std::array<std::optional<glm::vec3>,55>;
inline HumanPositions humanPositions(const Model& model,const std::vector<std::optional<Matrix>>* pose=nullptr) {
    HumanPositions result{};
    for(size_t i=0;i<model.bones.size();++i) {
        const int h=model.bones[i].human;
        if(h<0 || h>=55) continue;
        if(!pose) result[h]=vec(model.bones[i].position);
        else if((*pose)[i]) result[h]=glm::vec3(matrix(*(*pose)[i])[3]);
    }
    return result;
}
struct BodyLengths { float torso=0,legs=0; };

inline BodyLengths bodyLengths(const HumanPositions& p,const HumanPositions& other) {
    auto distance=[&](int a,int b) { return p[a] && p[b]?glm::length(*p[a]-*p[b]):0.f; };
    BodyLengths out; int previous=0;
    for(int h:{7,8,9,10}) if(p[h] && other[h]) { out.torso+=distance(previous,h); previous=h; }
    out.legs=.5f*(distance(1,3)+distance(3,5)+distance(2,4)+distance(4,6));
    return out;
}
struct ModelExtent { float bottom=0,top=0; float height() const { return top-bottom; } };


inline ModelExtent modelExtent(const Model& model) {
    const float inf=std::numeric_limits<float>::infinity();
    float bottom=inf,top=-inf;
    for(size_t bone=0;bone<model.bones.size();++bone) {
        const auto& b=model.bones[bone];
        if(b.human!=5 && b.human!=6) continue;
        for(bool strict:{true,false}) {
            float sole=inf;
            for(const auto& v:model.vertices) {
                float weight=0,maximum=0;
                for(size_t j=0;j<4;++j) { if(v.bones[j]==bone) weight+=v.weights[j]; maximum=std::max(maximum,v.weights[j]); }
                if(strict?weight<.8f:!(weight>0 && weight>=maximum)) continue;
                const auto local=glm::vec3(matrix(b.inverseBind)*glm::vec4(vec(v.position),1));
                sole=std::min(sole,(vec(b.position)+quat(b.rotation)*local).y);
            }
            if(std::isfinite(sole)) { bottom=std::min(bottom,sole); break; }
        }
    }
    for(const auto& part:model.parts) if(model.materials[part.material].name!="halo")
        for(size_t i=part.firstVertex;i<part.firstVertex+part.vertexCount;++i) top=std::max(top,model.vertices[i].position.y);
    return std::isfinite(bottom) && std::isfinite(top) && top>bottom?ModelExtent{bottom,top}:ModelExtent{};
}

inline float modelFit(BodyLengths model,BodyLengths host,float visualHeight,float targetHeight=0,float bodyRatio=1.3f) {
    float result=visualHeight>.001f
        ? (std::isfinite(targetHeight) && targetHeight>0?targetHeight:(host.torso+host.legs)*bodyRatio)/visualHeight
        : (host.torso+host.legs)/(model.torso+model.legs);
    if(!std::isfinite(result) || result<=0) throw std::runtime_error("Invalid model dimensions");
    return result;
}


inline glm::vec3 fittedPelvis(const HumanPositions& host,const HumanPositions& model,
        BodyLengths lengths,BodyLengths hostLengths,float scale,bool rabbit) {
    auto p=host[0].value();
    if(rabbit || !host[5] || !host[6] || !model[5] || !model[6] || hostLengths.legs<=.001f) return p;
    const float floor=std::min(host[5]->y,host[6]->y);
    const bool nearRoot=floor>hostLengths.legs*-.05f && floor<hostLengths.legs*.25f;
    const float correction=nearRoot?0.f:hostLengths.legs*-.095f+scale*std::min(model[5]->y,model[6]->y);
    p.y=floor+(p.y-floor)*(lengths.legs*scale/hostLengths.legs)+correction;
    return p;
}
}
