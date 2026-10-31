#include "sdk/model_bindings.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
struct Method {
    const char* name="Get";
    uint32_t token=0x06000001,flags=0;
    bool generic=false;
    const char* result="UnityEngine.GameObject";
    std::vector<const char*> parameters{"System.Int32"};
};
struct Class;
struct Field {
    uint32_t token=0x04000001,flags=1;
    const char* type="System.Collections.Generic.Dictionary`2<System.UInt32,Actor>";
    Class* owner=nullptr;
} field;
struct Class {
    const char* name="UIReference";
    uint32_t token=0x02000001;
    std::vector<Method*> methods;
    Class* parent=nullptr;
};
struct Image { const char* name;Class* klass; };
Method get,duplicate;
Class reference,decoy;
Method reflectedGet{"GetField",0,0,false,"System.Reflection.FieldInfo",{"System.String","System.Reflection.BindingFlags"}};
Method reflectedToken{"get_MetadataToken",0,0,false,"System.Int32",{}};
Class runtimeType{"System.RuntimeType",0,{&reflectedGet}},runtimeField{"System.Reflection.RuntimeFieldInfo",0,{&reflectedToken}};
Class intClass{"System.Int32",0,{}},stringClass{"System.String",0,{}};
struct Object { Class* klass;const char* text=nullptr;uint32_t token=0; };
Object reflectedOwner{&runtimeType},reflectedField{&runtimeField},fieldString{&stringClass},tokenBox{&intClass};
std::map<uint32_t,void*> roots;
uint32_t nextRoot=1;
int unavailableTokenRequests=0,reflectionCalls=0,retainCalls=0,failRetainAt=0;
enum class ReflectionFailure { None,GetException,GetNull,TokenException,TokenNull,BoxType,BoxSize,Unbox,Virtual };
ReflectionFailure reflectionFailure=ReflectionFailure::None;
Image external{"PluginsInit_300.dll",&reference},other{"Other.dll",&decoy};
const char* imageName(void* image) { return static_cast<Image*>(image)->name; }
void* classFromName(void* image,const char* ns,const char* name) {
    auto c=static_cast<Image*>(image)->klass;
    return !*ns && std::strcmp(name,c->name)==0?c:nullptr;
}
uint32_t classToken(void* c) { return static_cast<Class*>(c)->token; }
void* classParent(void* c) { return static_cast<Class*>(c)->parent; }
void* classType(void* c) { return const_cast<char*>(static_cast<Class*>(c)->name); }
void* methods(void* c,void** iterator) {
    auto& list=static_cast<Class*>(c)->methods;
    auto index=reinterpret_cast<uintptr_t>(*iterator);
    if(index>=list.size()) return nullptr;
    *iterator=reinterpret_cast<void*>(index+1);return list[index];
}
const char* methodName(void* m) { return static_cast<Method*>(m)->name; }
uint32_t methodToken(void* m) { return static_cast<Method*>(m)->token; }
uint32_t methodFlags(void* m,uint32_t*) { return static_cast<Method*>(m)->flags; }
bool methodGeneric(void* m) { return static_cast<Method*>(m)->generic; }
uint32_t methodCount(void* m) { return uint32_t(static_cast<Method*>(m)->parameters.size()); }
void* methodResult(void* m) { return const_cast<char*>(static_cast<Method*>(m)->result); }
void* methodParameter(void* m,uint32_t i) { return const_cast<char*>(static_cast<Method*>(m)->parameters.at(i)); }
char* typeName(void* type) {
    const auto raw=static_cast<const char*>(type);auto copy=static_cast<char*>(std::malloc(std::strlen(raw)+1));
    std::strcpy(copy,raw);return copy;
}
void* getField(void*,const char* name) { return std::strcmp(name,"actors")==0?&field:nullptr; }
void* fieldParent(void* f) { return static_cast<Field*>(f)->owner; }
const char* fieldName(void*) { return "actors"; }
uint32_t fieldFlags(void* f) { return static_cast<Field*>(f)->flags; }
void* fieldType(void* f) { return const_cast<char*>(static_cast<Field*>(f)->type); }
void check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void* typeObject(void* type) {
    check(type==reference.name,"Reflection used an unverified owner type");return &reflectedOwner;
}
void* objectClass(void* object) { return static_cast<Object*>(object)->klass; }
void* virtualMethod(void*,void* method) { return reflectionFailure==ReflectionFailure::Virtual?nullptr:method; }
void* stringNew(const char* text) { fieldString.text=text;return &fieldString; }
uint32_t retain(void* object,bool pinned) {
    check(!pinned,"Unexpected pinned reflection root");
    if(++retainCalls==failRetainAt) return 0;
    auto handle=nextRoot++;roots[handle]=object;return handle;
}
void release(uint32_t handle) { roots.erase(handle); }
bool rooted(void* object) {
    for(auto entry:roots) if(entry.second==object) return true;
    return false;
}
void* invoke(void* method,void* object,void** args,void** exception) {
    ++reflectionCalls;
    check(rooted(object),"Reflection receiver was not GC rooted");
    if(method==&reflectedGet) {
        check(object==&reflectedOwner && args && args[0]==&fieldString && rooted(args[0]),"GetField reference argument ABI/root mismatch");
        check(*static_cast<int32_t*>(args[1])==62,"GetField must include DeclaredOnly/public/nonpublic/static/instance");
        check(std::strcmp(fieldString.text,"actors")==0,"GetField received wrong field name");
        if(reflectionFailure==ReflectionFailure::GetException) { *exception=&reflectedOwner;return nullptr; }
        return reflectionFailure==ReflectionFailure::GetNull?nullptr:&reflectedField;
    }
    check(method==&reflectedToken && object==&reflectedField && !args,"MetadataToken invocation ABI mismatch");
    if(reflectionFailure==ReflectionFailure::TokenException) { *exception=&reflectedOwner;return nullptr; }
    tokenBox.klass=reflectionFailure==ReflectionFailure::BoxType?&stringClass:&intClass;
    tokenBox.token=field.token;
    return reflectionFailure==ReflectionFailure::TokenNull?nullptr:&tokenBox;
}
int32_t valueSize(void*,uint32_t*) { return reflectionFailure==ReflectionFailure::BoxSize?8:4; }
void* unbox(void* object) {
    check(rooted(object),"Token box was not GC rooted");
    return reflectionFailure==ReflectionFailure::Unbox?nullptr:&static_cast<Object*>(object)->token;
}
template<class F> void reject(F action,const char* message) {
    try { action(); } catch(const std::exception&) { return; }
    throw std::runtime_error(message);
}
void metadataTests() {
    using namespace ft::sdk;
    reference.methods={&get};decoy.methods={&duplicate};field.owner=&reference;
    Symbol s;s.assembly=external.name;s.type=reference.name;s.member=get.name;
    s.result=get.result;s.parameters={"System.Int32"};
    std::vector<void*> images{&other,&external};
    check(resolveType(images,s)==&reference,"Resolved a same-name type in another assembly");
    check(resolveMethod(images,s)==&get,"External token-zero method did not resolve");
    s.typeToken=0x02000002;
    reject([&]{resolveType(images,s);},"Wrong class token accepted");s.typeToken=0;
    s.assembly="Absent.dll";
    reject([&]{resolveType(images,s);},"Missing assembly accepted");s.assembly=external.name;
    s.token=0x06000002;
    reject([&]{resolveMethod(images,s);},"Wrong method token accepted");s.token=0;
    s.parameters={"System.String"};
    reject([&]{resolveMethod(images,s);},"Wrong parameter type accepted");s.parameters={"System.Int32"};
    s.result="System.Void";
    reject([&]{resolveMethod(images,s);},"Wrong result type accepted");s.result=get.result;
    get.flags=0x10;
    reject([&]{resolveMethod(images,s);},"Static method accepted as instance method");get.flags=0;
    get.generic=true;
    reject([&]{resolveMethod(images,s);},"Generic method accepted");get.generic=false;
    reference.methods.push_back(&duplicate);
    reject([&]{resolveMethod(images,s);},"Ambiguous token-zero overload accepted");
    duplicate.token=0x06000002;s.token=get.token;
    check(resolveMethod(images,s)==&get,"Method token did not disambiguate metadata");
    reference.methods.resize(1);
    Symbols bindings;bindings.types["reference"]=s;
    auto f=s;f.member="actors";f.token=field.token;f.flags=1;
    f.signature="class ['mscorlib']'System.Collections.Generic.Dictionary`2'<uint32, class 'Actor'>";
    bindings.fields["actors"]=f;bindings.methods["Get"]=s;
    validateBindings(images,bindings);
    check(resolveField(images,f)==&field,"Shared field resolver failed without native field-token API");
    check(reflectionCalls==4 && roots.empty(),"Successful reflection validation leaked roots or skipped token calls");
    ++field.token;
    reject([&]{validateBindings(images,bindings);},"Wrong reflected field token accepted");--field.token;
    field.owner=&decoy;
    reject([&]{validateBindings(images,bindings);},"Inherited/wrong-owner field accepted");field.owner=&reference;
    field.flags=0x11;
    reject([&]{validateBindings(images,bindings);},"Wrong field storage flags accepted");field.flags=1;
    const auto originalType=field.type;
    field.type="System.UInt32";
    reject([&]{validateBindings(images,bindings);},"Wrong field type accepted");
    field.type=originalType;
    runtimeField.methods.clear();
    reject([&]{resolveField(images,f);},"Missing MetadataToken accessor accepted");
    runtimeField.methods={&reflectedToken};
    runtimeType.methods.clear();
    reject([&]{resolveField(images,f);},"Missing reflection GetField accessor accepted");
    runtimeType.methods={&reflectedGet};
    reflectedToken.result="System.UInt32";
    reject([&]{resolveField(images,f);},"Wrong token accessor signature accepted");reflectedToken.result="System.Int32";
    reflectedToken.flags=0x10;
    reject([&]{resolveField(images,f);},"Static token accessor accepted");reflectedToken.flags=0;
    reflectedToken.generic=true;
    reject([&]{resolveField(images,f);},"Generic token accessor accepted");reflectedToken.generic=false;
    auto duplicateToken=reflectedToken;runtimeField.methods.push_back(&duplicateToken);
    reject([&]{resolveField(images,f);},"Ambiguous token accessor accepted");runtimeField.methods.resize(1);
    for(auto failure:{ReflectionFailure::GetException,ReflectionFailure::GetNull,ReflectionFailure::TokenException,
                     ReflectionFailure::TokenNull,ReflectionFailure::BoxType,ReflectionFailure::BoxSize,
                     ReflectionFailure::Unbox,ReflectionFailure::Virtual}) {
        reflectionFailure=failure;
        reject([&]{resolveField(images,f);},"Failed reflection operation accepted");
        check(roots.empty(),"Failed reflection operation leaked roots");
    }
    reflectionFailure=ReflectionFailure::None;
    for(int stage=1;stage<=4;++stage) {
        retainCalls=0;failRetainAt=stage;
        reject([&]{resolveField(images,f);},"Failed GC root creation accepted");
        check(roots.empty(),"Partial reflection root creation leaked roots");
    }
    failRetainAt=0;
    check(resolveField(images,f)==&field && roots.empty(),"Field resolver did not recover after reflection failures");
    check(unavailableTokenRequests==0,"Resolver still requested nonexistent il2cpp_field_get_token");
    std::cout<<"PASS: strict metadata and reflected field-token validation without native accessor\n";
}
}
namespace ft::il2cpp {
void* lookup(const char* name) {
#define API(key,fn) if(std::strcmp(name,"il2cpp_" key)==0) return reinterpret_cast<void*>(fn)
    API("image_get_name",imageName);API("class_from_name",classFromName);API("class_get_type_token",classToken);
    API("class_get_methods",methods);API("method_get_token",methodToken);API("method_get_name",methodName);
    API("method_get_flags",methodFlags);API("method_is_generic",methodGeneric);API("method_get_param_count",methodCount);
    API("method_get_return_type",methodResult);API("method_get_param",methodParameter);
    API("type_get_name",typeName);API("free",std::free);
    API("class_get_field_from_name",getField);API("field_get_parent",fieldParent);API("field_get_name",fieldName);
    API("field_get_flags",fieldFlags);API("field_get_type",fieldType);
    API("class_get_parent",classParent);API("class_get_type",classType);API("type_get_object",typeObject);
    API("object_get_class",objectClass);API("object_get_virtual_method",virtualMethod);API("runtime_invoke",invoke);
    API("string_new",stringNew);API("object_unbox",unbox);API("class_value_size",valueSize);
    API("gchandle_new",retain);API("gchandle_free",release);
    if(std::strcmp(name,"il2cpp_field_get_token")==0) ++unavailableTokenRequests;
#undef API
    return nullptr;
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::strcmp(argv[1],"--metadata")==0) metadataTests();
        else if(argc==2) {
            ft::sdk::loadConfig(argv[1]);
            check(!ft::sdk::assemblyName().empty(),"Missing assembly");
            check(!ft::sdk::modelBindings().methods.empty(),"Missing model methods");
            check(!ft::sdk::modelBindings().types.count("Driver") &&
                  !ft::sdk::modelBindings().methods.count("Driver.LateUpdate") &&
                  !ft::sdk::modelBindings().methods.count("ProfileUI.Update"),"Obsolete lifecycle mappings still gate rendering");
            check(!ft::sdk::ownPlayerBindings().fields.empty(),"Missing own-player fields");
            check(!ft::sdk::settingsBindings().types.empty(),"Missing settings types");
            reject([&]{ft::sdk::loadConfig(argv[1]);},"Configuration reload accepted");
        } else throw std::runtime_error("Pass config.json path or --metadata");
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
