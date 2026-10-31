#pragma once
#include "self_model/model_pose.h"

namespace ft::self_model {
struct SnapshotTransform {
    Vec3 position,scale;
    Quat rotation;
};



inline std::optional<SnapshotTransform> snapshotTransform(const glm::mat4& world) {
    if(!finite(glm::vec3(world[3]))) return {};
    glm::mat3 axes(world);
    glm::vec3 scale;
    for(int i=0;i<3;++i) {
        scale[i]=glm::length(axes[i]);
        if(!std::isfinite(scale[i]) || scale[i]<1e-6f) return {};
        axes[i]/=scale[i];
    }
    if(std::abs(glm::dot(axes[0],axes[1]))>1e-5f ||
       std::abs(glm::dot(axes[0],axes[2]))>1e-5f ||
       std::abs(glm::dot(axes[1],axes[2]))>1e-5f) return {};
    if(glm::determinant(axes)<0) { axes[0]=-axes[0]; scale.x=-scale.x; }
    return SnapshotTransform{vec(glm::vec3(world[3])),vec(scale),quat(glm::normalize(glm::quat_cast(axes)))};
}
}
