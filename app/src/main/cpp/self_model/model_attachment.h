#pragma once
#include "self_model/model_format.h"
#include <cmath>

namespace ft::self_model {
inline bool samePosition(Vec3 a,Vec3 b) {
    return std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.z-b.z)<=1e-5f;
}
struct AttachmentPosition {
    Vec3 original,last;
    bool changed=false;
    Vec3 baseline(Vec3 current) {
        if(!changed || !samePosition(current,last)) original=current;
        return original;
    }
};
}
