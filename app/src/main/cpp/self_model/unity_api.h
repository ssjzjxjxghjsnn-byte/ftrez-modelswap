#pragma once
#include "il2cpp/unity_resolve.h"
#include "il2cpp/method_invoke.h"
#include "self_model/model_format.h"
#include "self_model/model_array_buffer.h"
#include <cstring>
#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ft::self_model {
struct UnityApi {
    void* (*class_from_name)(void*,const char*,const char*)=nullptr;
    void* (*class_get_methods)(void*,void**)=nullptr;
    void* (*class_get_parent)(void*)=nullptr;
    const char* (*method_get_name)(void*)=nullptr;
    uint32_t (*method_get_param_count)(void*)=nullptr;
    void* (*method_get_param)(void*,uint32_t)=nullptr;
    void* (*method_get_return_type)(void*)=nullptr;
    uint32_t (*method_get_flags)(void*,uint32_t*)=nullptr;
    bool (*method_is_generic)(void*)=nullptr;
    char* (*type_get_name)(void*)=nullptr;
    void (*free)(void*)=nullptr;
    void* (*class_get_type)(void*)=nullptr;
    void* (*type_get_object)(void*)=nullptr;
    void* (*object_new)(void*)=nullptr;
    void* (*object_unbox)(void*)=nullptr;
    int32_t (*class_value_size)(void*,uint32_t*)=nullptr;
    void* (*array_new)(void*,uintptr_t)=nullptr;
    uint32_t (*array_length)(void*)=nullptr;
    int32_t (*array_element_size)(void*)=nullptr;
    uint32_t (*array_object_header_size)()=nullptr;
    void* (*object_get_class)(void*)=nullptr;
    void* (*string_new)(const char*)=nullptr;
    int32_t (*string_length)(void*)=nullptr;
    const char16_t* (*string_chars)(void*)=nullptr;
    uint32_t (*gchandle_new)(void*,bool)=nullptr;
    void* (*gchandle_get_target)(uint32_t)=nullptr;
    void (*gchandle_free)(uint32_t)=nullptr;
    void (*gc_wbarrier_set_field)(void*,void**,void*)=nullptr;
    std::vector<uint32_t> temporaries;
    uint64_t invokeCount=0,arrayCount=0,arrayBytes=0;
    std::map<std::string,void*> classes;
    std::map<void*,std::string> methodNames;
    void bind(const std::vector<void*>& images) {
#define API(n) n=reinterpret_cast<decltype(n)>(unity::resolve_api("il2cpp_" #n)); if(!n) throw std::runtime_error("Missing il2cpp_" #n)
        API(class_from_name); API(class_get_methods); API(class_get_parent); API(method_get_name);
        API(method_get_param_count); API(method_get_param); API(method_get_return_type); API(method_get_flags);
        API(method_is_generic); API(type_get_name); API(free); API(class_get_type); API(type_get_object);
        API(object_new); API(object_unbox); API(class_value_size); API(array_new); API(array_length); API(array_element_size);
        API(array_object_header_size); API(object_get_class); API(string_new); API(string_length); API(string_chars);
        API(gchandle_new); API(gchandle_get_target); API(gchandle_free); API(gc_wbarrier_set_field);
#undef API
        for(auto name:{"UnityEngine.Object","UnityEngine.GameObject","UnityEngine.Component","UnityEngine.Transform",
                "UnityEngine.Renderer","UnityEngine.SkinnedMeshRenderer","UnityEngine.MeshFilter","UnityEngine.MeshRenderer",
                "UnityEngine.Mesh","UnityEngine.Material","UnityEngine.Shader","UnityEngine.Texture","UnityEngine.Texture2D","UnityEngine.ImageConversion",
                "UnityEngine.Vector3","UnityEngine.Vector2","UnityEngine.Color32","UnityEngine.Matrix4x4","UnityEngine.BoneWeight",
                "UnityEngine.Animator","UnityEngine.Resources","UnityEngine.AssetBundle","UnityEngine.Time","UnityEngine.Camera",
                "UnityEngine.Rendering.RenderPipelineManager","UnityEngine.Rendering.ScriptableRenderContext","System.Byte","System.Int32"}) {
            std::string s=name; auto dot=s.rfind('.'); void* found=nullptr;
            for(auto image:images) if(auto c=class_from_name(image,s.substr(0,dot).c_str(),s.substr(dot+1).c_str())) {
                if(found && found!=c) throw std::runtime_error("Ambiguous Unity class: "+s); found=c;
            }
            classes[s]=found;
        }
    }
    void* klass(const std::string& name) {
        auto i=classes.find(name);
        if(i==classes.end() || !i->second) throw std::runtime_error("Missing class: "+name);
        return i->second;
    }
    std::string typeName(void* t) {
        char* s=type_get_name(t); if(!s) return {}; std::string v=s; free(s); return v;
    }
    void* method(const char* owner,const char* name,std::initializer_list<const char*> params,
                 const char* result,bool isStatic=false,bool optional=false) {
        auto k=classes[owner];
        for(size_t depth=0;k && depth<64;++depth,k=class_get_parent(k)) {
            void* it=nullptr; void* found=nullptr;
            while(auto m=class_get_methods(k,&it)) {
                if(method_is_generic(m) || std::strcmp(method_get_name(m),name) ||
                    method_get_param_count(m)!=params.size() || bool(method_get_flags(m,nullptr)&0x10)!=isStatic ||
                    typeName(method_get_return_type(m))!=result) continue;
                size_t i=0; bool match=true;
                for(auto p:params) if(typeName(method_get_param(m,i++))!=p) { match=false; break; }
                if(match) { if(found) throw std::runtime_error(std::string("Ambiguous method: ")+name); found=m; }
            }
            if(found) { methodNames[found]=std::string(owner)+"."+name; return found; }
        }
        if(optional) return nullptr;
        throw std::runtime_error(std::string("Missing method: ")+owner+"."+name);
    }
    void* hold(void* obj) {
        if(obj) { auto h=gchandle_new(obj,false); if(!h) throw std::runtime_error("Cannot root Unity object"); temporaries.push_back(h); }
        return obj;
    }
    void clearTemps() { for(auto h:temporaries) gchandle_free(h); temporaries.clear(); }
    void* call(void* m,void* self=nullptr,std::initializer_list<void*> args={}) {
        if(!m) throw std::runtime_error("Unity method unavailable");
        ++invokeCount;


        std::array<void*,8> local{};
        std::vector<void*> large;
        std::span<void*> parameters;
        if(args.size()<=local.size()) {
            std::copy(args.begin(),args.end(),local.begin());
            parameters={local.data(),args.size()};
        } else { large.assign(args); parameters=large; }
        auto r=il2cpp::Invoke(m,self,parameters);
        if(!r.succeeded()) {
            std::string detail=methodNames[m]+": invoke error "+std::to_string(int(r.error));
            if(r.exception) {
                hold(r.exception);
                // Native formatter avoids a second managed invocation while
                // reporting an Assembly.Load/type-initializer/interop failure.
                using Format=void(*)(const void*,char*,int);
                if(auto format=reinterpret_cast<Format>(unity::resolve_api("il2cpp_format_exception"))) {
                    std::array<char,4096> buffer{};
                    format(r.exception,buffer.data(),int(buffer.size())); buffer.back()=0;
                    detail+="; "; detail+=buffer.data();
                }
            }
            throw std::runtime_error(detail);
        }
        return hold(r.value);
    }
    template<class T> T value(void* m,void* self=nullptr,std::initializer_list<void*> args={}) {
        auto box=call(m,self,args); auto p=box?object_unbox(box):nullptr;
        if(!p) throw std::runtime_error("Unity returned no value");
        uint32_t align=0;
        if(class_value_size(object_get_class(box),&align)!=sizeof(T)) throw std::runtime_error("Unity value ABI mismatch");
        T v; std::memcpy(&v,p,sizeof v); return v;
    }
    void* text(const std::string& s) { return hold(string_new(s.c_str())); }
    void* type(const char* name) { return hold(type_get_object(class_get_type(klass(name)))); }
    std::string name(void* s) {
        if(!s) return {}; auto n=string_length(s); if(n<0 || n>512) return {};
        std::string v; auto p=string_chars(s);
        for(int i=0;i<n;++i) v+=p[i]<128?char(p[i]):'?'; return v;
    }
    template<class T> void* array(const char* name,std::span<const T> v) {
        auto a=hold(array_new(klass(name),v.size()));
        if(!a || array_element_size(object_get_class(a))!=sizeof(T)) throw std::runtime_error("Unity array ABI mismatch");
        ++arrayCount; arrayBytes+=v.size_bytes();
        auto dst=reinterpret_cast<uint8_t*>(a)+array_object_header_size();
        if(!v.empty()) std::memcpy(dst,v.data(),v.size_bytes()); return a;
    }
    template<class T> void* array(const char* name,const T* data,size_t count) {
        return array<T>(name,std::span<const T>(data,count));
    }
    template<class T> void* writeArray(void* a,std::span<const T> values) {


        return writeModelArray(*this,a,values.data(),values.size());
    }
    void* refs(const char* name,const std::vector<void*>& v) {
        auto a=hold(array_new(klass(name),v.size()));
        if(!a || array_element_size(object_get_class(a))!=sizeof(void*)) throw std::runtime_error("Unity reference array ABI mismatch");
        ++arrayCount; arrayBytes+=v.size()*sizeof(void*);
        auto dst=reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(a)+array_object_header_size());
        for(size_t i=0;i<v.size();++i) gc_wbarrier_set_field(a,dst+i,v[i]); return a;
    }
    template<class T> std::vector<T> elements(void* a,size_t limit) {
        if(!a) return {}; auto n=array_length(a);
        if(n>limit || array_element_size(object_get_class(a))!=sizeof(T)) throw std::runtime_error("Invalid Unity array");
        std::vector<T> v(n); if(n) std::memcpy(v.data(),reinterpret_cast<uint8_t*>(a)+array_object_header_size(),n*sizeof(T));
        return v;
    }
};
}
