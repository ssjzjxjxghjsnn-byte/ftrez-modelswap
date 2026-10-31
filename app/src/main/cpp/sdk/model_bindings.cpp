#include "sdk/model_bindings.h"
#include "il2cpp/unity_resolve.h"
#include "third_party/nlohmann/json.hpp"
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace ft::sdk {
namespace {
using Json=nlohmann::json;
struct Config { std::string assembly;Symbols model,ownPlayer,settings; };
std::unique_ptr<const Config> configuration;
const Config& config() {
    if(!configuration) throw std::runtime_error("Load resource config.json before binding the game");
    return *configuration;
}
std::string requiredText(const Json& object,const char* key) {
    const auto text=object.at(key).get<std::string>();
    if(text.empty() || text.size()>1024 || text.find('\0')!=std::string::npos)
        throw std::runtime_error(std::string("Invalid config string: ")+key);
    return text;
}
uint32_t metadataToken(const Json& value,uint8_t table,bool external=false) {
    uint64_t result=0;
    if(value.is_string()) {
        const auto text=value.get<std::string>();size_t used=0;
        result=std::stoull(text,&used,0);
        if(used!=text.size()) throw std::runtime_error("Invalid metadata token in config.json");
    } else if(value.is_number_unsigned() || value.is_number_integer()) result=value.get<uint64_t>();
    else throw std::runtime_error("Metadata token must be an integer or hexadecimal string");
    if(result==0 && external) return 0;
    if(result>UINT32_MAX || (result>>24)!=table || !(result&0xffffff))
        throw std::runtime_error("Metadata token table mismatch in config.json");
    return uint32_t(result);
}
Symbols section(const Json& json,const std::string& assembly) {
    Symbols out;
    if(!json.is_object() || !json.at("types").is_object() ||
       !json.at("fields").is_object() || !json.at("methods").is_object())
        throw std::runtime_error("Binding sections must contain types, fields and methods objects");
    for(const auto& [key,value]:json.at("types").items()) {
        Symbol s;s.type=requiredText(value,"name");s.namespc=value.value("namespace",std::string{});
        if(s.namespc.size()>1024 || s.namespc.find('\0')!=std::string::npos) throw std::runtime_error("Invalid configured namespace");
        s.assembly=value.value("assembly",assembly);
        s.typeToken=s.token=metadataToken(value.at("token"),2,s.assembly!=assembly);
        if(s.assembly.empty() || s.assembly.size()>1024 || s.assembly.find('\0')!=std::string::npos) throw std::runtime_error("Invalid configured assembly");
        out.types.emplace(key,std::move(s));
    }
    auto member=[&](const Json& value) {
        const auto owner=requiredText(value,"owner");
        const auto found=out.types.find(owner);
        if(found==out.types.end()) throw std::runtime_error("Configured member owner is missing: "+owner);
        Symbol s=found->second;s.member=requiredText(value,"name");return s;
    };
    for(const auto& [key,value]:json.at("fields").items()) {
        auto s=member(value);s.token=metadataToken(value.at("token"),4);
        s.signature=requiredText(value,"type");
        const auto& flags=value.at("flags");
        if(!flags.is_number_integer() || flags.get<int64_t>()<0 || flags.get<uint64_t>()>UINT32_MAX)
            throw std::runtime_error("Invalid configured field flags");
        s.flags=flags.get<uint32_t>();
        out.fields.emplace(key,std::move(s));
    }
    for(const auto& [key,value]:json.at("methods").items()) {
        auto s=member(value);s.token=metadataToken(value.at("token"),6,s.assembly!=assembly);
        s.result=requiredText(value,"return_type");s.parameters=value.at("parameters").get<std::vector<std::string>>();
        s.isStatic=value.at("static").get<bool>();
        for(const auto& parameter:s.parameters) if(parameter.empty() || parameter.size()>1024 || parameter.find('\0')!=std::string::npos)
            throw std::runtime_error("Invalid configured parameter type");
        out.methods.emplace(key,std::move(s));
    }
    if(out.types.empty()) throw std::runtime_error("Empty binding section in config.json");
    return out;
}
template<class Map> void requireKeys(const Map& values,std::initializer_list<const char*> keys) {
    for(auto key:keys) if(!values.count(key)) throw std::runtime_error(std::string("Missing config binding: ")+key);
}
std::string normalizedType(std::string value) {
    for(auto prefix:{"class ","valuetype "}) {
        size_t at=0;while((at=value.find(prefix,at))!=std::string::npos) value.erase(at,std::strlen(prefix));
    }
    size_t at=0;
    while((at=value.find("['",at))!=std::string::npos) {
        const auto end=value.find("']",at);if(end==std::string::npos) break;value.erase(at,end+2-at);
    }
    std::string out;
    for(size_t i=0;i<value.size();++i) {
        const auto c=value[i];
        if(c=='`') { while(i+1<value.size() && std::isdigit(static_cast<unsigned char>(value[i+1]))) ++i;continue; }
        if(c!='\'' && !std::isspace(static_cast<unsigned char>(c))) out+=c;
    }
    for(auto pair:{std::pair{"uint32","System.UInt32"},std::pair{"uint64","System.UInt64"},std::pair{"bool","System.Boolean"}}) {
        at=0;while((at=out.find(pair.first,at))!=std::string::npos) { out.replace(at,std::strlen(pair.first),pair.second);at+=std::strlen(pair.second); }
    }
    return out;
}
template<class T> T metadataApi(const char* name) {
    auto result=reinterpret_cast<T>(unity::resolve_api(name));
    if(!result) throw std::runtime_error(std::string("Binding validation API unavailable: ")+name);
    return result;
}
void* configuredType(const std::vector<void*>& images,const Symbol& s) {
    auto imageName=metadataApi<const char*(*)(void*)>("il2cpp_image_get_name");
    auto classFromName=metadataApi<void*(*)(void*,const char*,const char*)>("il2cpp_class_from_name");
    auto classToken=metadataApi<uint32_t(*)(void*)>("il2cpp_class_get_type_token");
    void* selected=nullptr;
    for(auto image:images) {
        const auto name=imageName(image);
        if(name && s.assembly==name) {
            if(selected && selected!=image) throw std::runtime_error("Ambiguous configured assembly: "+s.assembly);
            selected=image;
        }
    }
    if(!selected) throw std::runtime_error("Configured assembly unavailable: "+s.assembly);
    auto klass=classFromName(selected,s.namespc.c_str(),s.type.c_str());
    if(!klass || (s.typeToken && classToken(klass)!=s.typeToken))
        throw std::runtime_error("Configured type profile mismatch: "+s.type);
    return klass;
}
struct MemberApi {
    char* (*typeName)(void*)=metadataApi<decltype(typeName)>("il2cpp_type_get_name");
    void (*free)(void*)=metadataApi<decltype(free)>("il2cpp_free");
    std::string type(void* value) const {
        char* raw=typeName(value);
        if(!raw) throw std::runtime_error("Configured member type unavailable");
        std::string name=raw;free(raw);return normalizedType(std::move(name));
    }
};
struct MethodApi:MemberApi {
    void* (*methods)(void*,void**)=metadataApi<decltype(methods)>("il2cpp_class_get_methods");
    uint32_t (*token)(void*)=metadataApi<decltype(token)>("il2cpp_method_get_token");
    const char* (*name)(void*)=metadataApi<decltype(name)>("il2cpp_method_get_name");
    uint32_t (*flags)(void*,uint32_t*)=metadataApi<decltype(flags)>("il2cpp_method_get_flags");
    bool (*generic)(void*)=metadataApi<decltype(generic)>("il2cpp_method_is_generic");
    uint32_t (*count)(void*)=metadataApi<decltype(count)>("il2cpp_method_get_param_count");
    void* (*result)(void*)=metadataApi<decltype(result)>("il2cpp_method_get_return_type");
    void* (*parameter)(void*,uint32_t)=metadataApi<decltype(parameter)>("il2cpp_method_get_param");
    void* resolve(void* klass,const Symbol& s,bool required=true) const {
        void* iterator=nullptr;void* match=nullptr;
        while(auto method=methods(klass,&iterator)) {
            const auto methodName=name(method);
            if(!methodName || s.member!=methodName || (s.token && token(method)!=s.token) ||
               bool(flags(method,nullptr)&0x10)!=s.isStatic || generic(method) ||
               count(method)!=s.parameters.size() || type(result(method))!=normalizedType(s.result)) continue;
            bool parameters=true;
            for(size_t i=0;i<s.parameters.size();++i)
                if(type(parameter(method,uint32_t(i)))!=normalizedType(s.parameters[i])) parameters=false;
            if(parameters) {
                if(match) throw std::runtime_error("Ambiguous configured method: "+s.type+"."+s.member);
                match=method;
            }
        }
        if(!match && required) throw std::runtime_error("Configured method profile mismatch: "+s.type+"."+s.member);
        return match;
    }
};
struct FieldApi:MethodApi {
    void* (*field)(void*,const char*)=metadataApi<decltype(field)>("il2cpp_class_get_field_from_name");
    void* (*fieldParent)(void*)=metadataApi<decltype(fieldParent)>("il2cpp_field_get_parent");
    const char* (*fieldName)(void*)=metadataApi<decltype(fieldName)>("il2cpp_field_get_name");
    uint32_t (*fieldFlags)(void*)=metadataApi<decltype(fieldFlags)>("il2cpp_field_get_flags");
    void* (*fieldType)(void*)=metadataApi<decltype(fieldType)>("il2cpp_field_get_type");
    void* (*classType)(void*)=metadataApi<decltype(classType)>("il2cpp_class_get_type");
    void* (*typeObject)(void*)=metadataApi<decltype(typeObject)>("il2cpp_type_get_object");
    void* (*objectClass)(void*)=metadataApi<decltype(objectClass)>("il2cpp_object_get_class");
    void* (*parent)(void*)=metadataApi<decltype(parent)>("il2cpp_class_get_parent");
    void* (*virtualMethod)(void*,void*)=metadataApi<decltype(virtualMethod)>("il2cpp_object_get_virtual_method");
    void* (*invoke)(void*,void*,void**,void**)=metadataApi<decltype(invoke)>("il2cpp_runtime_invoke");
    void* (*stringNew)(const char*)=metadataApi<decltype(stringNew)>("il2cpp_string_new");
    void* (*unbox)(void*)=metadataApi<decltype(unbox)>("il2cpp_object_unbox");
    int32_t (*valueSize)(void*,uint32_t*)=metadataApi<decltype(valueSize)>("il2cpp_class_value_size");
    uint32_t (*retain)(void*,bool)=metadataApi<decltype(retain)>("il2cpp_gchandle_new");
    void (*release)(uint32_t)=metadataApi<decltype(release)>("il2cpp_gchandle_free");
    struct Root {
        void* object;
        uint32_t handle;
        const FieldApi& api;
        Root(const FieldApi& a,void* value):object(value),handle(value?a.retain(value,false):0),api(a) {
            if(!handle) throw std::runtime_error("Cannot retain field reflection object");
        }
        ~Root() { api.release(handle); }
        Root(const Root&)=delete;
        Root& operator=(const Root&)=delete;
    };
    void* reflectionCall(void* object,const char* name,const char* result,
                         std::vector<std::string> parameters,void** arguments=nullptr) const {
        Symbol signature;signature.member=name;signature.result=result;
        signature.parameters=std::move(parameters);
        void* method=nullptr;
        for(auto klass=objectClass(object);klass && !method;klass=parent(klass))
            method=MethodApi::resolve(klass,signature,false);
        if(!method) throw std::runtime_error(std::string("Field reflection method unavailable: ")+name);
        method=virtualMethod(object,method);
        if(!method) throw std::runtime_error(std::string("Field reflection virtual method unavailable: ")+name);
        void* exception=nullptr;
        auto value=invoke(method,object,arguments,&exception);
        if(exception) throw std::runtime_error(std::string("Field reflection call failed: ")+name);
        if(!value) throw std::runtime_error(std::string("Field reflection returned null: ")+name);
        return value;
    }
    uint32_t reflectedToken(void* klass,const Symbol& s) const {
        // Unity 2022.3 exports no field_get_token or field_get_object. Use its
        // supported managed reflection surface, without FieldInfo offsets.
        Root owner(*this,typeObject(classType(klass)));
        Root name(*this,stringNew(s.member.c_str()));
        // BindingFlags: DeclaredOnly | Instance | Static | Public | NonPublic.
        int32_t bindingFlags=2|4|8|16|32;
        void* arguments[]={name.object,&bindingFlags};
        Root reflected(*this,reflectionCall(owner.object,"GetField","System.Reflection.FieldInfo",
            {"System.String","System.Reflection.BindingFlags"},arguments));
        Root boxed(*this,reflectionCall(reflected.object,"get_MetadataToken","System.Int32",{}));
        auto boxedClass=objectClass(boxed.object);
        uint32_t alignment=0;
        if(!boxedClass || type(classType(boxedClass))!="System.Int32" ||
           valueSize(boxedClass,&alignment)!=sizeof(int32_t))
            throw std::runtime_error("Field reflection token is not an Int32");
        const auto data=unbox(boxed.object);
        if(!data) throw std::runtime_error("Field reflection token cannot be unboxed");
        uint32_t result=0;std::memcpy(&result,data,sizeof(result));return result;
    }
    void* resolveField(void* klass,const Symbol& s) const {
        auto value=field(klass,s.member.c_str());
        const auto name=value?fieldName(value):nullptr;
        if(!value || fieldParent(value)!=klass || !name || s.member!=name ||
           fieldFlags(value)!=s.flags || type(fieldType(value))!=normalizedType(s.signature) ||
           reflectedToken(klass,s)!=s.token)
            throw std::runtime_error("Configured field profile mismatch: "+s.type+"."+s.member);
        return value;
    }
};
}
void loadConfig(const std::string& file) {
    if(configuration) throw std::runtime_error("Binding configuration is already in use");
    std::ifstream input(file,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot open model resource config.json: "+file);
    input.seekg(0,std::ios::end);const auto size=input.tellg();input.seekg(0);
    if(size<=0 || size>1024*1024) throw std::runtime_error("Invalid config.json size");
    Json json;
    try { json=Json::parse(input); } catch(const std::exception& e) { throw std::runtime_error(std::string("Invalid config.json: ")+e.what()); }
    if(!json.is_object() || !json.at("schema_version").is_number_integer() || json.at("schema_version")!=1)
        throw std::runtime_error("Unsupported config.json schema_version");
    auto next=std::make_unique<Config>();next->assembly=requiredText(json,"assembly");
    next->model=section(json.at("model"),next->assembly);
    // Schema 1 files from the lifecycle-hook build remain usable. These
    // obsolete mappings must not gate the AOT render-frame pump.
    next->model.methods.erase("Driver.LateUpdate");
    next->model.methods.erase("ProfileUI.Update");
    next->model.types.erase("Driver");
    next->ownPlayer=section(json.at("own_player"),next->assembly);
    next->settings=section(json.at("settings"),next->assembly);
    requireKeys(next->model.types,{"BaseActor","BaseUI","ClothUpdater","ClothOwner","ProfileUI","TimelineBinder","LobbyUI"});
    requireKeys(next->model.fields,{"BaseActor.ClothOwner","ClothOwner.ClothState","BaseUI.Binder","ProfileUI.IsSelf","ClothUpdater.First","ClothUpdater.Arms","ClothUpdater.Shown","LobbyUI.Camera","LobbyUI.Host"});
    requireKeys(next->model.methods,{"TimelineBinder.MainBone"});
    requireKeys(next->ownPlayer.types,{"UGCTerrain","UGCActor","BaseActor","IdentityModules","IdentityScene"});
    requireKeys(next->ownPlayer.fields,{"UGCTerrain.minstance","UGCTerrain.ActorDic","UGCTerrain.curMapData","BaseActor.Character","BaseActor.isDestroyed","BaseActor.IdentityID","IdentityModules.Scene","IdentityScene.SelfID"});
    requireKeys(next->settings.types,{"SettingsUI","ToggleItem","UIReference"});
    requireKeys(next->settings.fields,{"graphicsReference","donorRow"});
    requireKeys(next->settings.methods,{"referenceGet"});
    configuration=std::move(next);
}
const std::string& assemblyName() { return config().assembly; }
const Symbols& modelBindings() { return config().model; }
const Symbols& ownPlayerBindings() { return config().ownPlayer; }
const Symbols& settingsBindings() { return config().settings; }
void* resolveType(const std::vector<void*>& images,const Symbol& symbol) { return configuredType(images,symbol); }
void* resolveMethod(const std::vector<void*>& images,const Symbol& symbol) {
    return MethodApi{}.resolve(configuredType(images,symbol),symbol);
}
void* resolveField(const std::vector<void*>& images,const Symbol& symbol) {
    return FieldApi{}.resolveField(configuredType(images,symbol),symbol);
}

void validateBindings(const std::vector<void*>& images,const Symbols& symbols) {
    MethodApi api;
    for(const auto& [key,s]:symbols.types) configuredType(images,s);
    if(!symbols.fields.empty()) {
        FieldApi fields;
        for(const auto& [key,s]:symbols.fields) fields.resolveField(configuredType(images,s),s);
    }
    for(const auto& [key,s]:symbols.methods) api.resolve(configuredType(images,s),s);
}
}
