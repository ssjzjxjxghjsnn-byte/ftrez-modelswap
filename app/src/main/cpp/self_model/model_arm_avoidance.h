#pragma once
#include "self_model/model_pose.h"
#include <map>

namespace ft::self_model {


class ArmAvoidance {
    struct Section { float y; glm::vec2 center,radius; };
    struct Profile { size_t bone; float low,high; std::vector<Section> sections; };
    std::vector<Profile> profiles_;
    std::vector<int> side_;
    std::array<int,6> arms_{};
    std::array<float,2> padding_{};
    static float quantile(std::vector<float>& values,float fraction) {
        const size_t at=std::min(values.size()-1,size_t(.5f+float(values.size()-1)*fraction));
        std::nth_element(values.begin(),values.begin()+at,values.end()); return values[at];
    }
    glm::vec3 displacement(const Model& model,const std::vector<PoseBone>& pose,glm::vec3 p,float padding) const {
        glm::vec3 best(0); float largest=0;
        for(const auto& profile:profiles_) {
            const auto& b=model.bones[profile.bone]; const auto& current=pose[profile.bone];
            const auto delta=quat(current.rotation)*glm::inverse(quat(b.rotation));
            const auto local=vec(b.position)+glm::inverse(delta)*(p-vec(current.position));
            if(local.y<profile.low || local.y>=profile.high) continue;
            auto section=profile.sections.back();
            if(local.y<=profile.sections.front().y) section=profile.sections.front();
            else for(size_t i=1;i<profile.sections.size();++i) if(local.y<profile.sections[i].y) {
                const auto& a=profile.sections[i-1]; const auto& b=profile.sections[i];
                const float t=(local.y-a.y)/std::max(b.y-a.y,1e-6f);
                section.center=glm::mix(a.center,b.center,t); section.radius=glm::mix(a.radius,b.radius,t); break;
            }
            const auto radius=section.radius+glm::vec2(padding);
            if(radius.x<=1e-6f || radius.y<=1e-6f) continue;
            const auto offset=glm::vec2(local.x,local.z)-section.center;
            const float normalized=glm::length(offset/radius);
            if(normalized>=1) continue;
            const auto target=section.center+(normalized>.001f?offset/normalized:glm::vec2(0,radius.y));
            const auto push=delta*glm::vec3(target.x-local.x,0,target.y-local.z);
            const float depth=glm::length(push);
            if(depth>largest) { largest=depth; best=push; }
        }
        return best;
    }
public:
    void clear() { profiles_.clear(); side_.clear(); arms_.fill(-1); padding_.fill(0); }
    size_t profileCount() const { return profiles_.size(); }
    void prepare(const Model& model) {
        clear(); side_.assign(model.bones.size(),-1);
        std::vector<int> torso(model.bones.size(),-1);
        for(size_t i=0;i<model.bones.size();++i) {
            const auto& b=model.bones[i];
            if(b.human==0 || b.human==7 || b.human==8 || b.human==54) torso[i]=int(i);
            else if(b.human<0 && b.parent>=0 && !b.dynamics && !(b.flags&0xf0)) torso[i]=torso[b.parent];
            if(b.human==13 || b.human==14) side_[i]=b.human-13;
            else if(b.parent>=0) side_[i]=side_[b.parent];
            if(b.human>=13 && b.human<=18) arms_[b.human-13]=int(i);
        }
        struct Point { glm::vec3 p; int anchor; };
        std::vector<Point> points; std::array<std::vector<float>,2> radii;
        for(const auto& part:model.parts) if(!(part.flags&2)) {
            for(size_t i=part.firstVertex;i<part.firstVertex+part.vertexCount;++i) {
                const auto& v=model.vertices[i];
                const auto at=size_t(std::max_element(v.weights.begin(),v.weights.end())-v.weights.begin());
                if(v.weights[at]<=0) continue;
                const auto bone=v.bones[at]; const auto p=vec(v.position);
                if(torso[bone]>=0) points.push_back({p,torso[bone]});
                const int side=side_[bone];
                if(side<0 || arms_[2+side]<0 || arms_[4+side]<0) continue;
                const auto start=vec(model.bones[arms_[2+side]].position);
                const auto axis=vec(model.bones[arms_[4+side]].position)-start;
                const float t=glm::dot(p-start,axis)/std::max(glm::dot(axis,axis),1e-12f);
                if(t>.2f && t<.8f) radii[side].push_back(glm::length(p-start-t*axis));
            }
        }
        for(size_t side=0;side<2;++side) if(!radii[side].empty()) {
            auto& r=radii[side]; const auto middle=r.begin()+r.size()/2;
            std::nth_element(r.begin(),middle,r.end()); padding_[side]=*middle*.8f;
        }
        if(points.size()<64) return;
        float low=points.front().p.y,high=low;
        for(const auto& point:points) { low=std::min(low,point.p.y); high=std::max(high,point.p.y); }
        const float step=(high-low)/16;
        if(step<=1e-6f) return;
        for(int slice=0;slice<16;++slice) {
            const float bottom=low+slice*step,top=slice==15?high+1e-6f:bottom+step;
            std::vector<float> xs,zs; std::map<int,size_t> counts;
            for(const auto& point:points) if(point.p.y>=bottom && point.p.y<top) {
                xs.push_back(point.p.x); zs.push_back(point.p.z); ++counts[point.anchor];
            }
            if(xs.size()<12) continue;
            const auto anchor=std::max_element(counts.begin(),counts.end(),[](const auto& a,const auto& b){return a.second<b.second;})->first;
            const glm::vec2 minimum{quantile(xs,.02f),quantile(zs,.02f)},maximum{quantile(xs,.98f),quantile(zs,.98f)};
            auto found=std::find_if(profiles_.begin(),profiles_.end(),[&](const Profile& p){return p.bone==size_t(anchor);});
            if(found==profiles_.end()) { profiles_.push_back({size_t(anchor),bottom,top,{}}); found=profiles_.end()-1; }
            found->low=std::min(found->low,bottom); found->high=std::max(found->high,top);
            found->sections.push_back({(bottom+top)*.5f,(minimum+maximum)*.5f,(maximum-minimum)*.5f});
        }
    }
    void update(const Model& model,std::vector<PoseBone>& pose) const {
        if(profiles_.empty()) return;
        for(int side=0;side<2;++side) {
            if(padding_[side]<=0 || arms_[side]<0 || arms_[2+side]<0 || arms_[4+side]<0) continue;
            const auto pivot=vec(pose[arms_[side]].position); float usedAngle=0;
            for(int iteration=0;iteration<2;++iteration) {
                const auto elbow=vec(pose[arms_[2+side]].position),hand=vec(pose[arms_[4+side]].position);
                glm::vec3 sample(0),push(0); float deepest=0;
                for(float t:{0.f,.35f,.7f,1.f,1.3f}) {
                    const auto p=glm::mix(elbow,hand,t),d=displacement(model,pose,p,padding_[side]);
                    const float depth=glm::length(d);
                    if(depth>deepest) { deepest=depth; sample=p; push=d; }
                }
                if(deepest<=0) break;
                const auto from=sample-pivot,to=sample+push-pivot,axis=glm::cross(from,to);
                const float sine=glm::length(axis),angle=std::min(std::atan2(sine,glm::dot(from,to)),1.05f-usedAngle);
                if(glm::length(from)<1e-5f || glm::length(to)<1e-5f || sine<1e-8f || angle<=.0001f) break;
                const auto q=glm::angleAxis(angle,axis/sine); usedAngle+=angle;
                for(size_t i=0;i<pose.size();++i) if(side_[i]==side) {
                    pose[i].position=vec(pivot+q*(vec(pose[i].position)-pivot));
                    pose[i].rotation=quat(glm::normalize(q*quat(pose[i].rotation)));
                }
            }
        }
    }
};
}
