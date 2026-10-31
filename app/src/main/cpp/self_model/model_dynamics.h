#pragma once
#include "self_model/model_pose.h"
#include <algorithm>

namespace ft::self_model {
inline glm::vec3 limitCone(glm::vec3 rest,glm::vec3 wanted,float angle) {
    if(std::acos(std::clamp(glm::dot(rest,wanted),-1.f,1.f))<=angle) return wanted;
    const auto axis=direction(glm::cross(rest,wanted),perpendicular(rest));
    return glm::angleAxis(angle,axis)*rest;
}


struct SpringParameters { float damping,attraction,cone,translation,padding; };
inline constexpr SpringParameters springParameters[]{
    {0,0,0,1,0}, {.15f,.10f,.52f,.75f,.012f},
    {.25f,.30f,.26f,.90f,.010f}, {.25f,.25f,.30f,.90f,.014f}
};

class ModelDynamics {
    struct Tail { glm::vec3 current{0},previous{0}; bool ready=false; };
    struct Skirt { glm::vec3 radial{0},axis{0}; float rest=0,limit=0; bool enabled=false; };
    std::vector<Tail> tails_;
    std::vector<Skirt> skirts_;
    int pelvis_=-1,leftThigh_=-1,rightThigh_=-1;
    glm::vec3 leftDirection_{0},rightDirection_{0},previousRoot_{0};
    float previousScale_=0;
    bool ready_=false;

    glm::quat skirtRotation(size_t i,const Model& m,const std::vector<PoseBone>& world,
                            glm::quat base) const {
        const auto& s=skirts_[i]; if(!s.enabled) return base;
        const auto delta=quat(world[pelvis_].rotation)*glm::inverse(quat(m.bones[pelvis_].rotation));
        float angle=s.rest;
        for(auto side:{std::pair{leftThigh_,leftDirection_},std::pair{rightThigh_,rightDirection_}}) {
            const auto d=glm::inverse(delta)*(quat(world[side.first].rotation)*side.second);
            const float weight=std::clamp(1.f-std::abs(glm::dot(s.axis,d))/.8f,0.f,1.f);


            const float margin=(m.bones[i].flags&128)? .03f : .2f;
            const float wanted=std::atan2(glm::dot(s.radial,d),-d.y)+margin;
            if(wanted>s.rest) angle=std::max(angle,s.rest+(wanted-s.rest)*weight);
        }
        angle=std::min(angle,s.limit);
        if(angle<=s.rest+.0001f) return base;
        return glm::normalize(glm::angleAxis(angle-s.rest,delta*s.axis)*base);
    }

    glm::quat spring(size_t i,const Model& m,const std::vector<PoseBone>& world,
                     glm::vec3 origin,glm::quat rotation,float scale,float step,glm::vec3 movement) {
        const auto& b=m.bones[i]; auto& history=tails_[i];
        const auto& p=springParameters[b.dynamics];
        const float length=glm::length(vec(b.tail))*scale;
        if(!std::isfinite(length) || length<1e-6f) { history.ready=false; return rotation; }
        const auto rest=direction(rotation*vec(b.tail));
        const auto target=origin+rest*length;
        if(!history.ready) { history={target,target,true}; movement={0,0,0}; }
        const auto inherited=movement*p.translation;
        const auto current=history.current+inherited,previous=history.previous+inherited;
        auto predicted=current+(current-previous)*std::pow(1.f-p.damping,step);
        predicted+=(target-predicted)*(1.f-std::pow(1.f-p.attraction,step));
        const float cone=p.cone*(b.parent>=0 && m.bones[b.parent].dynamics?1.f:.5f);
        auto tail=origin+limitCone(rest,direction(predicted-origin,rest),cone)*length;


        for(int pass=0;pass<3;++pass) {
            bool hit=false;
            for(const auto& c:m.colliders) {
                if(!(c.mask&(1u<<b.dynamics)) || c.bone==int(i) || c.end==int(i)) continue;
                auto center=vec(world[c.bone].position);
                glm::vec3 segment(0);
                if(c.localCapsule) {
                    const auto anchorRotation=quat(world[c.bone].rotation);
                    center+=anchorRotation*(vec(c.offset)*scale);
                    segment=anchorRotation*((vec(c.endOffset)-vec(c.offset))*scale);
                } else if(c.end<0) center+=quat(world[c.bone].rotation)*(vec(c.offset)*scale);
                else segment=vec(world[c.end].position)-center;
                if(c.localCapsule || c.end>=0) {
                    const float t=std::clamp(glm::dot(tail-center,segment)/std::max(glm::dot(segment,segment),1e-12f),0.f,1.f);
                    center+=segment*t;
                }
                const float radius=(c.radius+p.padding)*scale;
                if(glm::length(tail-center)<radius) {
                    tail=center+direction(tail-center,rest)*radius; hit=true;
                }
            }
            tail=origin+direction(tail-origin,rest)*length;
            if(!hit) break;
        }
        if(!finite(tail)) { history.ready=false; return rotation; }
        history={tail,current,true};
        return glm::normalize(between(rest,tail-origin)*rotation);
    }
public:
    void clear() { tails_.clear(); skirts_.clear(); ready_=false; previousScale_=0; }
    void reset() { for(auto& t:tails_) t.ready=false; ready_=false; }
    void prepare(const Model& m) {
        clear(); tails_.resize(m.bones.size()); skirts_.resize(m.bones.size());
        auto human=[&](int slot) { for(size_t i=0;i<m.bones.size();++i) if(m.bones[i].human==slot) return int(i); return -1; };
        pelvis_=human(0); leftThigh_=human(1); rightThigh_=human(2);
        const int leftCalf=human(3),rightCalf=human(4);
        if(pelvis_<0 || leftThigh_<0 || rightThigh_<0 || leftCalf<0 || rightCalf<0) return;
        auto leg=[&](int thigh,int calf) {
            return glm::inverse(quat(m.bones[thigh].rotation))*direction(vec(m.bones[calf].position)-vec(m.bones[thigh].position));
        };
        leftDirection_=leg(leftThigh_,leftCalf); rightDirection_=leg(rightThigh_,rightCalf);
        for(size_t i=0;i<m.bones.size();++i) {
            const auto& b=m.bones[i];
            if(!(b.flags&0xf0) || b.parent!=pelvis_) continue;
            int child=-1; float reach=0;
            for(size_t j=i+1;j<m.bones.size();++j) {
                if(child<0 && m.bones[j].parent==int(i)) child=int(j);
                int ancestor=int(j); while(ancestor>int(i)) ancestor=m.bones[ancestor].parent;
                if(ancestor==int(i)) reach=std::max(reach,glm::length(vec(m.bones[j].position)-vec(b.position))+glm::length(vec(m.bones[j].tail)));
            }
            auto radial=vec(b.position)-vec(m.bones[pelvis_].position); radial.y=0;
            if(child<0 || glm::length(radial)<1e-5f) continue;
            radial=direction(radial);
            const auto d=direction(vec(m.bones[child].position)-vec(b.position));
            skirts_[i]={radial,direction(glm::cross(radial,glm::vec3(0,1,0))),
                std::atan2(glm::dot(radial,d),-d.y),(b.flags&128)?1.2f:(reach<=.26f?1.4f:std::min(std::asin(.26f/reach),1.4f)),true};
        }
    }


    void update(const Model& m,std::vector<PoseBone>& pose,const std::vector<std::optional<Matrix>>& mapped,
                const glm::mat4& rootToWorld,glm::vec3 hostPosition,float dt) {
        if(pose.size()!=m.bones.size() || mapped.size()!=pose.size()) throw std::runtime_error("Incomplete dynamics pose");
        if(tails_.size()!=pose.size()) prepare(m);
        const auto rootRotation=rotation(rootToWorld);
        const float scale=glm::length(glm::vec3(rootToWorld[0]));
        const auto inverse=glm::inverse(rootToWorld);
        if(!finite(hostPosition) || !std::isfinite(dt) || dt<0 || !std::isfinite(scale) || scale<1e-6f) { reset(); return; }
        auto movement=hostPosition-previousRoot_;
        if(!ready_ || dt>.25f || glm::length(movement)>2.f*scale || std::abs(scale-previousScale_)>1e-4f*scale) {
            reset(); movement={0,0,0};
        }
        const float step=std::clamp(dt*60.f,.25f,3.f);
        std::vector<PoseBone> animated=pose,world=pose;
        for(size_t i=0;i<pose.size();++i) {
            animated[i].position=vec(glm::vec3(rootToWorld*glm::vec4(vec(pose[i].position),1)));
            animated[i].rotation=quat(rootRotation*quat(pose[i].rotation));
        }


        world=animated;
        for(size_t i=0;i<pose.size();++i) {
            const auto& b=m.bones[i]; auto p=vec(animated[i].position); auto q=quat(animated[i].rotation);
            if(b.parent>=0) {
                const auto parentDelta=quat(world[b.parent].rotation)*glm::inverse(quat(animated[b.parent].rotation));
                p=vec(world[b.parent].position)+parentDelta*(p-vec(animated[b.parent].position));
                if(!mapped[i] || (b.flags&8)) q=parentDelta*q;
            }
            q=skirtRotation(i,m,world,q);
            if(b.dynamics) q=spring(i,m,world,p,q,scale,step,movement);
            world[i]={vec(p),quat(q)};
            pose[i]={vec(glm::vec3(inverse*glm::vec4(p,1))),quat(glm::normalize(glm::inverse(rootRotation)*q))};
        }
        previousRoot_=hostPosition; previousScale_=scale; ready_=true;
    }
};
}
