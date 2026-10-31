#include "self_model/lobby_render.h"
#include "self_model/unity_api.h"
#include "self_model/lobby_geometry_hooks.h"
#include "sdk/model_bindings.h"
#include <android/log.h>
#include <chrono>
#include <mutex>

extern "C" const unsigned char ft_lobby_code_start[], ft_lobby_code_end[];
extern "C" const unsigned char ft_lobby_shader_start[], ft_lobby_shader_end[];

namespace ft::self_model {
namespace {
std::mutex messageMutex;
std::string message="Lobby rendering: waiting for lobby";
std::string lastDetail;
void status(std::string value, std::string summary={}) {
    std::lock_guard lock(messageMutex);
    if(lastDetail!=value) __android_log_print(ANDROID_LOG_INFO,"FTREZ-LOBBY","%s",value.c_str());
    lastDetail=std::move(value);
    message=summary.empty()?lastDetail:std::move(summary);
}
struct Bridge {
    UnityApi api;
    void *update=nullptr,*getStatus=nullptr;
    void* watchInstantiation=nullptr;
    bool loaded=false,sent=false,lastEnabled=false;
    bool apiReady=false;
    void *lastSource=nullptr,*lastCamera=nullptr,*sourceHostField=nullptr,*sourceCameraField=nullptr;
    void *findObjects=nullptr,*isAlive=nullptr,*componentGO=nullptr,*activeHierarchy=nullptr,*cameraActive=nullptr;
    uint32_t sourceOwner=0;
    uint32_t (*weakHandle)(void*,bool)=nullptr;
    void (*readField)(void*,void*,void*)=nullptr;
    std::chrono::steady_clock::time_point nextSourceSearch{};
    std::array<LobbyRenderTarget,3> lastTargets;
    std::chrono::steady_clock::time_point retry{},nextStatus{},nextSourceRefresh{};

    void prepareApi(const std::vector<void*>& images) {
        if(apiReady) return;
        api.bind(images); apiReady=true;
    }
    void source(const std::vector<void*>& images,const sdk::Symbols& symbols,void*& host,void*& camera) {
        prepareApi(images);
        if(!sourceHostField) {
            if(!symbols.types.count("LobbyUI") || !symbols.fields.count("LobbyUI.Host") || !symbols.fields.count("LobbyUI.Camera"))
                throw std::runtime_error("Lobby UI binding profile unavailable");
            const auto& type=symbols.types.at("LobbyUI");
            auto klass=sdk::resolveType(images,type);
            api.classes["LobbyUI"]=klass;
            auto getField=reinterpret_cast<void*(*)(void*,const char*)>(unity::resolve_api("il2cpp_class_get_field_from_name"));
            auto fieldType=reinterpret_cast<void*(*)(void*)>(unity::resolve_api("il2cpp_field_get_type"));
            auto fieldFlags=reinterpret_cast<uint32_t(*)(void*)>(unity::resolve_api("il2cpp_field_get_flags"));
            readField=reinterpret_cast<decltype(readField)>(unity::resolve_api("il2cpp_field_get_value"));
            weakHandle=reinterpret_cast<decltype(weakHandle)>(unity::resolve_api("il2cpp_gchandle_new_weakref"));
            if(!getField || !fieldType || !fieldFlags || !readField || !weakHandle) throw std::runtime_error("Lobby field API unavailable");
            auto hostField=getField(klass,symbols.fields.at("LobbyUI.Host").member.c_str());
            auto cameraField=getField(klass,symbols.fields.at("LobbyUI.Camera").member.c_str());
            if(!hostField || !cameraField || (fieldFlags(hostField)&0x50) || (fieldFlags(cameraField)&0x50) ||
               api.typeName(fieldType(hostField))!="UnityEngine.Transform" || api.typeName(fieldType(cameraField))!="UnityEngine.Camera")
                throw std::runtime_error("Lobby field signature mismatch");
            findObjects=api.method("UnityEngine.Object","FindObjectsOfType",{"System.Type"},"UnityEngine.Object[]",true);
            isAlive=api.method("UnityEngine.Object","op_Implicit",{"UnityEngine.Object"},"System.Boolean",true);
            componentGO=api.method("UnityEngine.Component","get_gameObject",{},"UnityEngine.GameObject");
            activeHierarchy=api.method("UnityEngine.GameObject","get_activeInHierarchy",{},"System.Boolean");
            cameraActive=api.method("UnityEngine.Camera","get_isActiveAndEnabled",{},"System.Boolean");
            sourceCameraField=cameraField; sourceHostField=hostField;
        }
        auto active=[&](void* object) {
            return object && api.value<bool>(isAlive,nullptr,{object}) &&
                api.value<bool>(activeHierarchy,api.call(componentGO,object));
        };
        auto owner=api.hold(sourceOwner?api.gchandle_get_target(sourceOwner):nullptr);
        if(!active(owner)) {
            if(sourceOwner) api.gchandle_free(sourceOwner);
            sourceOwner=0; owner=nullptr;
            const auto now=std::chrono::steady_clock::now();
            if(now<nextSourceSearch) return;
            nextSourceSearch=now+std::chrono::milliseconds(500);
            for(auto object:api.elements<void*>(api.call(findObjects,nullptr,{api.type("LobbyUI")}),128)) if(active(object)) {
                if(owner) throw std::runtime_error("Multiple active Lobby UIs");
                owner=object;
            }
            if(owner) {
                sourceOwner=weakHandle(owner,false);
                if(!sourceOwner) throw std::runtime_error("Cannot retain Lobby UI");
            }
        }
        if(!owner) return;
        void *h=nullptr,*c=nullptr;
        readField(owner,sourceHostField,&h); api.hold(h);
        readField(owner,sourceCameraField,&c); api.hold(c);
        if(active(h) && active(c) && api.value<bool>(cameraActive,c)) { host=h; camera=c; }
    }
    void bind(const std::vector<void*>& images) {
        prepareApi(images);
        status("Lobby rendering: loading managed helper");
        void* assemblyClass=nullptr;
        for(auto image:images) if(auto k=api.class_from_name(image,"System.Reflection","Assembly")) {
            if(assemblyClass && assemblyClass!=k) throw std::runtime_error("Ambiguous Assembly API");
            assemblyClass=k;
        }
        api.classes["System.Reflection.Assembly"]=assemblyClass;
        auto load=api.method("System.Reflection.Assembly","Load",{"System.Byte[]"},"System.Reflection.Assembly",true);
        auto domainGet=reinterpret_cast<void*(*)()>(unity::resolve_api("il2cpp_domain_get"));
        auto open=reinterpret_cast<void*(*)(void*,const char*)>(unity::resolve_api("il2cpp_domain_assembly_open"));
        auto getImage=reinterpret_cast<void*(*)(void*)>(unity::resolve_api("il2cpp_assembly_get_image"));
        if(!domainGet || !open || !getImage) throw std::runtime_error("Managed lobby assembly API unavailable");
        if(!loaded) {
            auto bytes=api.array<uint8_t>("System.Byte",std::span<const uint8_t>(
                ft_lobby_code_start,ft_lobby_code_end-ft_lobby_code_start));
            if(!api.call(load,nullptr,{bytes})) throw std::runtime_error("Lobby assembly could not load");
            loaded=true;
        }
        auto assembly=open(domainGet(),"FTRez.LobbyModel");
        auto image=assembly?getImage(assembly):nullptr;
        auto klass=image?api.class_from_name(image,"FTRez.LobbyModel","Entry"):nullptr;
        if(!klass) throw std::runtime_error("Lobby entry unavailable");
        api.classes["LobbyEntry"]=klass;
        auto initialize=api.method("LobbyEntry","Initialize",{"System.Byte[]"},"System.Void",true);
        auto target=api.method("LobbyEntry","SetTargets",{"UnityEngine.Transform","UnityEngine.Camera","UnityEngine.Transform[]","UnityEngine.Renderer[]","System.Int32[]","System.Boolean"},"System.Void",true);
        auto describe=api.method("LobbyEntry","GetStatus",{},"System.String",true);
        // Validate the whole bridge before Initialize installs render callbacks.
        auto setHooks=api.method("LobbyEntry","SetGeometryHooks",{"System.IntPtr"},"System.Void",true);
        watchInstantiation=api.method("LobbyEntry","WatchInstantiation",{"UnityEngine.AsyncOperation"},"System.Void",true);
        auto bundle=api.array<uint8_t>("System.Byte",std::span<const uint8_t>(
            ft_lobby_shader_start,ft_lobby_shader_end-ft_lobby_shader_start));
        api.call(initialize,nullptr,{bundle});
        auto& hooks=LobbyGeometryHooks::get();
        if(hooks.install()) {
            void* address=&hooks.state;
            api.call(setHooks,nullptr,{&address});
        }
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-LOBBY","geometry lifecycle hooks=%s",hooks.ready?"active":"fallback scan");
        update=target; getStatus=describe;
    }
    void drainInstantiations() {
        auto& hooks=LobbyGeometryHooks::get();
        if(!watchInstantiation || hooks.state.pending.load(std::memory_order_acquire)==0) return;
        std::vector<uint32_t> pending;
        { std::lock_guard lock(hooks.mutex); pending.swap(hooks.operations); }
        for(auto handle:pending) {
            try {
                auto object=api.hold(api.gchandle_get_target(handle));
                api.call(watchInstantiation,nullptr,{object});
                hooks.state.pending.fetch_sub(1,std::memory_order_release);
            } catch(...) { /* retain conservative checks if event binding failed */ }
            api.gchandle_free(handle);
        }
    }
};
Bridge& bridge() { static auto* value=new Bridge; return *value; }
}

std::string lobbyRenderStatus() {
    std::lock_guard lock(messageMutex);
    return message;
}

bool isLobbyModelHost(const std::vector<void*>& images,const sdk::Symbols& symbols,void* host) {
    if(!host) return false;
    auto& state=bridge();
    void *source=nullptr,*camera=nullptr;
    try { state.source(images,symbols,source,camera); }
    catch(const std::exception&) { source=nullptr; }
    const bool matches=source && source==host;
    if(state.api.gchandle_free) state.api.clearTemps();
    return matches;
}

void updateLobbyRender(const std::vector<void*>& images,const sdk::Symbols& symbols,bool inLobby,
                       const std::array<LobbyRenderTarget,3>& targets,bool enabled) {
    auto& state=bridge();
    bool any=false;
    for(const auto& target:targets) any|=target.host && !target.renderers.empty();
    if(!state.update && (!enabled || (!inLobby && !any))) {
        if(!enabled) status("Lobby rendering: off");
        return;
    }
    const auto now=std::chrono::steady_clock::now();
    if(now<state.retry) return;
    try {
        void *lobbySource=nullptr,*lobbyCamera=nullptr;
        std::string sourceError;
        // The metadata-bound UI/camera activity is the lobby signal. A stale match
        // flag after returning to a lobby page must not block recapture.
        if(enabled) {
            try { state.source(images,symbols,lobbySource,lobbyCamera); }
            catch(const std::exception& e) { sourceError=e.what(); }
        }
        if(!state.update) state.bind(images);
        state.drainInstantiations();
        bool changed=!state.sent || state.lastEnabled!=enabled || state.lastSource!=lobbySource || state.lastCamera!=lobbyCamera;
        for(size_t slot=0;slot<targets.size();++slot) {
            const auto& target=targets[slot]; const auto& previous=state.lastTargets[slot];
            changed|=target.host!=previous.host || target.renderers!=previous.renderers;
        }
        // Managed Entry retains the supplied objects. Compare identities only;
        // never dereference cached pointers. Clears and slot handoffs always
        // pass immediately, including a second update within the same frame.
        if(changed || (lobbySource && now>=state.nextSourceRefresh)) {
            std::vector<void*> hosts,renderers;
            std::vector<int32_t> owners;
            for(size_t slot=0;slot<targets.size();++slot) {
                const auto& target=targets[slot]; hosts.push_back(target.host);
                for(auto renderer:target.renderers) { renderers.push_back(renderer); owners.push_back(int32_t(slot)); }
            }
            auto sourceHosts=state.api.refs("UnityEngine.Transform",hosts);
            auto objects=state.api.refs("UnityEngine.Renderer",renderers);
            auto indices=state.api.array<int32_t>("System.Int32",std::span<const int32_t>(owners));
            state.api.call(state.update,nullptr,{lobbySource,lobbyCamera,sourceHosts,objects,indices,&enabled});
            state.lastTargets=targets; state.lastSource=lobbySource; state.lastCamera=lobbyCamera; state.lastEnabled=enabled; state.sent=true;
            state.nextSourceRefresh=now+std::chrono::milliseconds(500);
            if(changed) state.nextStatus={};
        }
        if(now>=state.nextStatus) {
            auto message=state.api.name(state.api.call(state.getStatus));
            if(!sourceError.empty()) message+=" | capture binding: "+sourceError;
            status(std::move(message));
            state.nextStatus=now+std::chrono::seconds(1);
        }
    } catch(const std::exception& e) {
        state.sent=false;
        const std::string detail=e.what();
        const auto start=detail.find("Lobby init [");
        const auto summary=start==std::string::npos?std::string("Lobby unavailable: ")+detail.substr(0,detail.find_first_of("\r\n")):
            detail.substr(start,detail.find_first_of("\r\n",start)-start);
        status(std::string("Lobby rendering unavailable: ")+detail,summary);
        state.retry=now+std::chrono::seconds(5);
    }
    if(state.api.gchandle_free) state.api.clearTemps();
}
}
