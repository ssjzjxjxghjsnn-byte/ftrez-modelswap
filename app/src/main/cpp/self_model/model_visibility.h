#pragma once
#include <cstdint>

namespace ft::self_model {


inline bool nearHead(bool hidden,float fromHost,float fromModel,float headRadius,float height) {
    return fromModel<headRadius*(hidden?2.1f:1.8f) || fromHost<height*(hidden?.26f:.22f);
}
struct SavedRenderer { uint32_t handle; bool enabled; bool changed=false; };
template<class Write> void restoreVisibility(SavedRenderer& saved,Write write) {
    if(!saved.changed) return;
    write(saved.enabled);
    saved.changed=false;
}
template<class Write> void applyVisibility(SavedRenderer& saved,bool current,bool hide,Write write) {
    if(!saved.changed) saved.enabled=current;
    else if(current) saved.enabled=true;
    if(hide && saved.enabled) {
        saved.changed=true;
        if(current) write(false);
    } else restoreVisibility(saved,write);
}
}
