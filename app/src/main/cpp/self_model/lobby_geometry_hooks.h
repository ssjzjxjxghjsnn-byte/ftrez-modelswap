#pragma once
#include "il2cpp/unity_resolve.h"
#include "dobby.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>
#include <android/log.h>

namespace ft::self_model {
// Signatures verified against the Unity 2022.3 CoreModule ISIL. No MethodInfo
// argument: these are resolved engine icalls, not generated AOT wrappers.
class LobbyGeometryHooks {
public:
    struct State { alignas(8) std::atomic<int64_t> generation{1}; std::atomic<int32_t> pending{0}; };
    static_assert(offsetof(State,pending)==8);
    State state;
    std::mutex mutex;
    std::vector<uint32_t> operations;
    uint32_t (*retain)(void*,bool)=nullptr;
    bool attempted=false,ready=false;
    static LobbyGeometryHooks& get() { static auto* h=new LobbyGeometryHooks; return *h; }
    void changed() { state.generation.fetch_add(1,std::memory_order_release); }

    template<int Id,class Signature> struct Tap;
    template<int Id,class R,class... Args> struct Tap<Id,R(Args...)> {
        inline static R (*original)(Args...)=nullptr;
        static R invoke(Args... args) {
            get().changed();
            // Invalidate before AND after: Awake/OnEnable can render reentrantly.
            if constexpr(std::is_void_v<R>) { original(args...); get().changed(); }
            else { R value=original(args...); get().changed(); return value; }
        }
    };
    inline static void (*setLayer)(void*,int32_t)=nullptr;
    inline static int32_t (*getLayer)(void*)=nullptr;
    static void layer(void* object,int32_t value) {
        const bool changed=getLayer(object)!=value;
        if(changed) get().changed();
        setLayer(object,value);
        if(changed) get().changed();
    }
    inline static void* (*instantiateAsync)(void*,int32_t,void*,void*,int32_t,void*,int32_t)=nullptr;
    static void* async(void* object,int32_t count,void* parameters,void* positions,int32_t np,void* rotations,int32_t nr) {
        auto& h=get(); h.state.pending.fetch_add(1,std::memory_order_release); h.changed();
        auto* result=instantiateAsync(object,count,parameters,positions,np,rotations,nr);
        if(result) {
            auto handle=h.retain(result,false);
            if(handle) { std::lock_guard lock(h.mutex); h.operations.push_back(handle); }
            // A failed handle deliberately leaves the conservative scan on.
        } else h.state.pending.fetch_sub(1,std::memory_order_release);
        h.changed(); return result;
    }
    template<class Fn> static bool hook(void* address,Fn replacement,Fn& original) {
        return address && DobbyHook(address,reinterpret_cast<dobby_dummy_func_t>(replacement),
            reinterpret_cast<dobby_dummy_func_t*>(&original))==0 && original;
    }
    template<int Id,class Signature,class Resolve> bool tap(Resolve resolve,const char* name) {
        const bool ok=hook(resolve(name),Tap<Id,Signature>::invoke,Tap<Id,Signature>::original);
        if(!ok) __android_log_print(ANDROID_LOG_WARN,"FTREZ-LOBBY","geometry hook unavailable: %s",name);
        return ok;
    }
    bool install() {
        if(attempted) return ready;
        attempted=true;
        auto resolve=reinterpret_cast<void*(*)(const char*)>(unity::resolve_api("il2cpp_resolve_icall"));
        retain=reinterpret_cast<decltype(retain)>(unity::resolve_api("il2cpp_gchandle_new"));
        if(!resolve || !retain) {
            __android_log_print(ANDROID_LOG_WARN,"FTREZ-LOBBY","geometry hook unavailable: icall/GC handle API");
            return false;
        }
        getLayer=reinterpret_cast<decltype(getLayer)>(resolve("UnityEngine.GameObject::get_layer"));
        if(!getLayer) {
            __android_log_print(ANDROID_LOG_WARN,"FTREZ-LOBBY","geometry hook unavailable: UnityEngine.GameObject::get_layer");
            return false;
        }
        // Every required hook must install before the managed cache is enabled.
        bool ok=hook(resolve("UnityEngine.GameObject::set_layer"),layer,setLayer);
        if(!ok) __android_log_print(ANDROID_LOG_WARN,"FTREZ-LOBBY","geometry hook unavailable: UnityEngine.GameObject::set_layer");
        ok=tap<0,void(void*,void*)>(resolve,"UnityEngine.GameObject::Internal_CreateGameObject") && ok;
        ok=tap<1,void*(void*,void*)>(resolve,"UnityEngine.GameObject::Internal_AddComponentWithType") && ok;
        ok=tap<2,void*(void*,void*)>(resolve,"UnityEngine.GameObject::AddComponentInternal") && ok;
        ok=tap<3,void*(void*)>(resolve,"UnityEngine.Object::Internal_CloneSingle") && ok;
        ok=tap<4,void*(void*,void*,bool)>(resolve,"UnityEngine.Object::Internal_CloneSingleWithParent") && ok;
        ok=tap<5,void*(void*,void*)>(resolve,"UnityEngine.Object::Internal_CloneSingleWithScene_Injected") && ok;
        ok=tap<6,void*(void*,void*)>(resolve,"UnityEngine.Object::Internal_CloneSingleWithParams_Injected") && ok;
        ok=tap<7,void*(void*,void*,void*)>(resolve,"UnityEngine.Object::Internal_InstantiateSingle_Injected") && ok;
        ok=tap<8,void*(void*,void*,void*,void*)>(resolve,"UnityEngine.Object::Internal_InstantiateSingleWithParent_Injected") && ok;
        ok=tap<9,void*(void*,void*,void*,void*)>(resolve,"UnityEngine.Object::Internal_InstantiateSingleWithParams_Injected") && ok;
        ok=tap<10,void(int32_t,void*,void*,int32_t,void*)>(resolve,"UnityEngine.GameObject::InstantiateGameObjects_Injected") && ok;
        const bool asyncOK=hook(resolve("UnityEngine.Object::Internal_InstantiateAsyncWithParams_Injected"),async,instantiateAsync);
        if(!asyncOK) __android_log_print(ANDROID_LOG_WARN,"FTREZ-LOBBY","geometry hook unavailable: UnityEngine.Object::Internal_InstantiateAsyncWithParams_Injected");
        ok=asyncOK && ok;
        ready=ok; return ready;
    }
};
}
