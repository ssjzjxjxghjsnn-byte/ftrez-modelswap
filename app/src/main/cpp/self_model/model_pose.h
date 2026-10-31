#pragma once
#include "self_model/model_format.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <optional>
#include <algorithm>
#include <stdexcept>

namespace ft::self_model {
inline glm::vec3 vec(Vec3 v) { return {v.x,v.y,v.z}; }
inline Vec3 vec(glm::vec3 v) { return {v.x,v.y,v.z}; }
inline glm::quat quat(Quat q) { return {q.w,q.x,q.y,q.z}; }
inline Quat quat(glm::quat q) { return {q.x,q.y,q.z,q.w}; }
inline glm::mat4 matrix(const Matrix& m) { return glm::make_mat4(m.m.data()); }
inline bool finite(glm::vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline glm::quat rotation(glm::mat4 m) {
    glm::mat3 basis{m};
    for(int i=0;i<3;++i) {
        auto n=glm::length(basis[i]);
        if(!std::isfinite(n) || n<1e-6f) throw std::runtime_error("Invalid host bone matrix");
        basis[i]/=n;
    }
    if(glm::determinant(basis)<.5f) throw std::runtime_error("Mirrored or invalid host skeleton");
    return glm::normalize(glm::quat_cast(basis));
}
inline glm::vec3 direction(glm::vec3 v,glm::vec3 fallback={0,1,0}) {
    const float n=glm::length(v);
    return std::isfinite(n) && n>1e-6f?v/n:fallback;
}
inline glm::vec3 perpendicular(glm::vec3 v) {
    return direction(glm::cross(v,std::abs(v.y)<.9f?glm::vec3(0,1,0):glm::vec3(1,0,0)));
}
inline glm::quat between(glm::vec3 from,glm::vec3 to) {
    from=direction(from); to=direction(to,from);
    const float dot=std::clamp(glm::dot(from,to),-1.f,1.f);
    if(dot>1.f-1e-7f) return {1,0,0,0};
    if(dot<-1.f+1e-6f) return glm::angleAxis(glm::pi<float>(),perpendicular(from));
    return glm::normalize(glm::quat(1.f+dot,glm::cross(from,to)));
}
struct PoseBone { Vec3 position; Quat rotation; };



inline std::vector<PoseBone> retarget(const Model& model,
        const std::vector<std::optional<Matrix>>& hostRest,
        const std::vector<std::optional<Matrix>>& hostNow) {
    if(hostRest.size()!=model.bones.size() || hostNow.size()!=model.bones.size())
        throw std::runtime_error("Incomplete pose arrays");



    glm::quat bindHeading{1,0,0,0};
    int left=-1,right=-1;
    for(size_t i=0;i<model.bones.size();++i) {
        if(model.bones[i].human==1 && hostRest[i]) left=int(i);
        if(model.bones[i].human==2 && hostRest[i]) right=int(i);
    }
    if(left>=0 && right>=0) {
        auto source=vec(model.bones[right].position)-vec(model.bones[left].position);
        auto target=glm::vec3(matrix(*hostRest[right])[3]-matrix(*hostRest[left])[3]);
        source.y=target.y=0;
        if(glm::length(source)>1e-5f && glm::length(target)>1e-5f)
            bindHeading=glm::angleAxis(std::atan2(source.z,source.x)-std::atan2(target.z,target.x),glm::vec3(0,1,0));
    }
    std::vector<PoseBone> out; out.reserve(model.bones.size());
    for(size_t i=0;i<model.bones.size();++i) {
        const auto& b=model.bones[i]; auto p=vec(b.position); auto q=quat(b.rotation);
        glm::quat parentDelta{1,0,0,0};
        if(b.parent>=0) {
            const auto& parent=model.bones[b.parent];
            parentDelta=quat(out[b.parent].rotation)*glm::inverse(quat(parent.rotation));
            p=vec(out[b.parent].position)+parentDelta*(p-vec(parent.position));
            q=parentDelta*q;
        }
        if(!(b.flags&8) && hostRest[i] && hostNow[i]) {
            auto rest=matrix(*hostRest[i]), now=matrix(*hostNow[i]);
            q=rotation(now)*glm::inverse(rotation(rest))*bindHeading*quat(b.rotation);


            if((b.flags&4) && !(b.flags&2)) {
                int target=b.aim;
                for(int depth=0;target>=0 && depth<8;++depth) {
                    if(hostNow[target]) break;
                    target=model.bones[target].aim;
                }
                auto local=vec(b.tail);
                if(target>=0 && hostNow[target])
                    local=glm::inverse(quat(b.rotation))*(vec(model.bones[target].position)-vec(b.position));
                else {
                    target=-1;
                    if(b.fingerTarget>=0) for(size_t j=0;j<model.bones.size();++j)
                        if(model.bones[j].human==b.fingerTarget && hostNow[j]) { target=int(j); break; }
                }
                if(target>=0) {
                    const auto wanted=glm::vec3(matrix(*hostNow[target])[3])-glm::vec3(now[3]);
                    if(glm::length(local)>1e-5f && glm::length(wanted)>1e-5f)
                        q=between(q*local,wanted)*q;
                }
            }
        }
        q=glm::normalize(q);
        if(!finite(p) || !std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)||!std::isfinite(q.w))
            throw std::runtime_error("Non-finite retargeted pose");
        out.push_back({vec(p),quat(q)});
    }
    return out;
}
inline std::vector<glm::mat4> skinMatrices(const Model& model,const std::vector<PoseBone>& pose) {
    std::vector<glm::mat4> result;
    result.reserve(pose.size());
    for(size_t i=0;i<pose.size();++i) {
        auto m=glm::mat4_cast(quat(pose[i].rotation)); m[3]=glm::vec4(vec(pose[i].position),1);
        m=glm::scale(m,vec(model.bones[i].scale));
        result.push_back(m*matrix(model.bones[i].inverseBind));
    }
    return result;
}
inline std::pair<Vec3,Vec3> skin(const Vertex& v,const std::vector<glm::mat4>& matrices) {
    glm::vec3 p{0},n{0};
    for(size_t j=0;j<4;++j) if(v.weights[j]) {
        const auto& m=matrices.at(v.bones[j]);
        p+=glm::vec3(m*glm::vec4(vec(v.position),1))*v.weights[j];
        n+=glm::mat3(m)*vec(v.normal)*v.weights[j];
    }
    if(glm::length(n)>1e-6f) n=glm::normalize(n);
    return {vec(p),vec(n)};
}
}
