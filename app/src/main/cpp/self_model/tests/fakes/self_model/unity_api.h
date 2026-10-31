#pragma once
// Minimal public-Unity boundary fake; the tests include the real ModelHooks.
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace profile_test {
struct Object { bool alive=true,active=true,own=false,enabled=true; };
enum class Method { Find,Alive,GameObject,Active,Frame };
inline std::map<Method,int> methods;
inline std::vector<void*> objects;
inline std::map<uint32_t,void*> handles;
inline uint32_t nextHandle=1;
inline int frame=1,queries=0,holds=0,clears=0,updates=0;
inline bool mainThread=true,failQuery=false,nullArray=false,failWeak=false;
inline std::function<void()> render;
inline uint32_t weak(void* object,bool) {
    if(failWeak) return 0;
    const auto handle=nextHandle++;
    handles[handle]=object;
    return handle;
}
inline void reset() {
    objects.clear();handles.clear();nextHandle=1;
    frame=1;queries=holds=clears=updates=0;
    mainThread=true;failQuery=nullArray=failWeak=false;render={};
}
inline void* method(Method id) { return &methods[id]; }
}

namespace unity {
inline void* resolve_api(const char* name) {
    return std::string(name)=="il2cpp_gchandle_new_weakref"?
        reinterpret_cast<void*>(&profile_test::weak):nullptr;
}
}

namespace ft::self_model {
struct UnityApi {
    std::map<std::string,void*> classes;
    void bind(const std::vector<void*>&) {}
    void* method(const char*,const char* name,std::initializer_list<const char*>,const char*,bool=false) {
        using profile_test::Method;
        const std::map<std::string,Method> names={
            {"FindObjectsOfType",Method::Find},{"op_Implicit",Method::Alive},
            {"get_gameObject",Method::GameObject},{"get_activeInHierarchy",Method::Active},
            {"get_frameCount",Method::Frame}};
        return profile_test::method(names.at(name));
    }
    void* type(const char* name) { return classes.at(name); }
    void* hold(void* object) { if(object) ++profile_test::holds;return object; }
    void clearTemps() { profile_test::holds=0;++profile_test::clears; }
    void gchandle_free(uint32_t handle) { profile_test::handles.erase(handle); }
    void* gchandle_get_target(uint32_t handle) {
        auto it=profile_test::handles.find(handle);
        return it==profile_test::handles.end()?nullptr:it->second;
    }
    void* call(void* method,void* self=nullptr,std::initializer_list<void*> = {}) {
        using namespace profile_test;
        if(method==profile_test::method(Method::Find)) {
            ++queries;
            if(failQuery) throw std::runtime_error("query failed");
            return nullArray?nullptr:hold(&objects);
        }
        if(method==profile_test::method(Method::GameObject)) return hold(self);
        throw std::runtime_error("unexpected reference call");
    }
    template<class T> T value(void* method,void* self=nullptr,std::initializer_list<void*> args={}) {
        using namespace profile_test;
        if(method==profile_test::method(Method::Frame)) return static_cast<T>(frame);
        if(method==profile_test::method(Method::Alive)) {
            auto object=static_cast<Object*>(*args.begin());
            return static_cast<T>(object && object->alive);
        }
        if(method==profile_test::method(Method::Active)) return static_cast<T>(static_cast<Object*>(self)->active);
        throw std::runtime_error("unexpected value call");
    }
    template<class T> std::vector<T> elements(void* array,size_t limit) {
        auto& values=*static_cast<std::vector<void*>*>(array);
        if(values.size()>limit) throw std::runtime_error("invalid array");
        return std::vector<T>(values.begin(),values.end());
    }
};
}
