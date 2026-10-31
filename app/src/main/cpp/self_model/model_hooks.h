#pragma once
#include "self_model/unity_api.h"
#include "sdk/model_bindings.h"
#include "il2cpp/frame_hook.h"
#include <android/log.h>
#include <chrono>
#include <functional>

namespace ft::self_model {


class ModelHooks {
    using Clock=std::chrono::steady_clock;
    UnityApi api;
    enum class Presence { Unknown, None, Active };
    Presence presence=Presence::Unknown;
    uint32_t profile=0;
    uint32_t (*weakHandle)(void*,bool)=nullptr;
    void *findProfiles=nullptr,*isAlive=nullptr,*componentGO=nullptr,*activeHierarchy=nullptr,*frameCount=nullptr;
    int lastQueryFrame=-1;
    bool installed=false;
    Clock::time_point nextSearch{},nextError{};
    std::function<bool()> onMainThread;
    std::function<void()> update;
    void releaseProfile() {
        if(profile) api.gchandle_free(profile);
        profile=0;
    }
    bool activeProfile(void* object) {
        return object && api.value<bool>(isAlive,nullptr,{object}) &&
            api.value<bool>(activeHierarchy,api.call(componentGO,object));
    }
    void refreshProfile(bool force=false,int frame=-1) {
        struct Temps { UnityApi& api;~Temps() { api.clearTemps(); } } temps{api};
        if(activeProfile(api.hold(profileObject()))) presence=Presence::Active;
        else if(profile) {
            // Losing a weak target is not evidence that no other page exists.
            releaseProfile();presence=Presence::Unknown;
        }
        const auto now=Clock::now();
        if(!force && now<nextSearch) return;
        if(frame<0) frame=api.value<int>(frameCount);
        if(frame==lastQueryFrame) return;
        // Reuse both successful and failed queries across cameras/snapshot
        // donors in this frame. Unknown always blocks lobby selection.
        lastQueryFrame=frame;
        nextSearch=now+std::chrono::milliseconds(500);
        releaseProfile();presence=Presence::Unknown;
        auto objects=api.call(findProfiles,nullptr,{api.type("ProfileUI")});
        if(!objects) throw std::runtime_error("Profile search returned no array");
        void* found=nullptr;bool ambiguous=false;
        for(auto object:api.elements<void*>(objects,128)) {
            if(!activeProfile(object)) continue;
            if(found) { ambiguous=true;break; }
            found=object;
        }
        presence=found?Presence::Active:Presence::None;
        if(found && !ambiguous) {
            profile=weakHandle(found,false);
            if(!profile) throw std::runtime_error("Cannot retain profile page");
        }
    }
    void refreshSafely(bool force=false,int frame=-1) {
        try { refreshProfile(force,frame); }
        catch(const std::exception& e) {
            releaseProfile();presence=Presence::Unknown;
            const auto now=Clock::now();
            if(now>=nextError) {
                nextError=now+std::chrono::seconds(5);
                __android_log_print(ANDROID_LOG_WARN,"FTREZ-MODEL","Profile lookup: %s",e.what());
            }
        }
    }
public:
    void* profileObject() {
        return profile?api.gchandle_get_target(profile):nullptr;
    }
    bool lobbyAllowed(int frame) {
        if(presence==Presence::Active) return false;
        // A cached empty result cannot rule out a page created during the
        // 500 ms polling window. Confirm absence before choosing a lobby host.
        refreshSafely(true,frame);
        return presence==Presence::None;
    }
    void install(const std::vector<void*>& images,const sdk::Symbols& symbols,
                 std::function<bool()> mainThread,std::function<void()> afterUpdate) {
        if(installed) return;
        api.bind(images);
        weakHandle=reinterpret_cast<decltype(weakHandle)>(unity::resolve_api("il2cpp_gchandle_new_weakref"));
        if(!weakHandle)
            throw std::runtime_error("Profile metadata/weak handle API unavailable");
        api.classes["ProfileUI"]=sdk::resolveType(images,symbols.types.at("ProfileUI"));
        findProfiles=api.method("UnityEngine.Object","FindObjectsOfType",{"System.Type"},"UnityEngine.Object[]",true);
        isAlive=api.method("UnityEngine.Object","op_Implicit",{"UnityEngine.Object"},"System.Boolean",true);
        componentGO=api.method("UnityEngine.Component","get_gameObject",{},"UnityEngine.GameObject");
        activeHierarchy=api.method("UnityEngine.GameObject","get_activeInHierarchy",{},"System.Boolean");
        frameCount=api.method("UnityEngine.Time","get_frameCount",{},"System.Int32",true);
        onMainThread=std::move(mainThread); update=std::move(afterUpdate);
        il2cpp::frame_hook::install(images,[this] {
            if(!onMainThread()) return;
            refreshSafely();
            update();
        });
        installed=true;
    }
};
}
