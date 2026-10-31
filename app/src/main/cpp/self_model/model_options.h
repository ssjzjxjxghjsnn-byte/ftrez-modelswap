#pragma once
#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

namespace ft {
struct SelfModelOptions {
    bool enabled = false, hideOriginal = true;
    float scale = 1, offset = 0;
    int model = 0, style = 2, hairTone = 0;
    bool rabbitEnabled = false;
    int rabbitModel = 0;
    bool lobbyRendering = true;
    bool operator==(const SelfModelOptions& b) const {
        return enabled == b.enabled && hideOriginal == b.hideOriginal &&
            scale == b.scale && offset == b.offset && model == b.model && style == b.style && hairTone == b.hairTone &&
            rabbitEnabled == b.rabbitEnabled && rabbitModel == b.rabbitModel &&
            lobbyRendering == b.lobbyRendering;
    }
};
inline SelfModelOptions normalizeSelfModel(SelfModelOptions v) {
    v.scale = std::isfinite(v.scale) ? std::clamp(v.scale,.3f,2.f) : 1.f;
    v.offset = std::isfinite(v.offset) ? std::clamp(v.offset,-.6f,.8f) : 0.f;
    v.model = std::max(v.model,0);
    v.style = std::clamp(v.style,0,4);
    v.hairTone = std::clamp(v.hairTone,0,3);
    v.rabbitModel = std::max(v.rabbitModel,0);
    return v;
}
inline SelfModelOptions modelInstanceOptions(SelfModelOptions v, bool rabbit) {
    v=normalizeSelfModel(v);
    if(rabbit) { v.enabled=v.rabbitEnabled; if(v.rabbitModel) v.model=v.rabbitModel-1; }
    return v;
}
struct SelfModelChoice { int key; std::string id,label; };
struct SelfModelControls {
    std::function<SelfModelOptions()> read;
    std::function<void(SelfModelOptions)> write;
    std::function<std::string()> status;
    std::function<std::vector<SelfModelChoice>()> catalog;
};
}
