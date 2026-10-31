#include "sdk/own_player.h"
#include "sdk/model_bindings.h"
#include "self_model/unity_api.h"
#include <cstring>
#include <map>
#include <stdexcept>

namespace ft::sdk {
struct OwnPlayer::Impl {
    self_model::UnityApi api;
    std::map<std::string,void*> fields;
    void* (*getField)(void*,const char*)=nullptr;
    void (*readField)(void*,void*,void*)=nullptr;
    void (*readStatic)(void*,void*)=nullptr;
    uint32_t (*fieldFlags)(void*)=nullptr;
    void* (*fieldType)(void*)=nullptr;
    int (*typeKind)(void*)=nullptr;
    size_t (*fieldOffset)(void*)=nullptr;
    void* (*elementClass)(void*)=nullptr;
    uint32_t (*objectHeaderSize)()=nullptr;
    struct DictionaryLayout { void *entries=nullptr,*count=nullptr,*version=nullptr; };
    std::map<void*,DictionaryLayout> dictionaries;
    bool bound=false;

    template<class T> T read(void* object,void* field) {
        T value{};
        if(object && field) readField(object,field,&value);
        return value;
    }
    template<class T> T read(void* object,const char* key) { return read<T>(object,fields.at(key)); }
    bool is(void* object,const char* key) {
        auto klass=object?api.object_get_class(object):nullptr;
        const auto expected=api.klass(key);
        for(unsigned depth=0;klass && depth<64;++depth,klass=api.class_get_parent(klass))
            if(klass==expected) return true;
        return false;
    }
    void bind(const std::vector<void*>& images) {
        api.bind(images);
#define API(target,name) target=reinterpret_cast<decltype(target)>(unity::resolve_api(name)); if(!target) throw std::runtime_error("Missing " name)
        API(getField,"il2cpp_class_get_field_from_name");
        API(readField,"il2cpp_field_get_value");
        API(readStatic,"il2cpp_field_static_get_value");
        API(fieldFlags,"il2cpp_field_get_flags");
        API(fieldType,"il2cpp_field_get_type");
        API(typeKind,"il2cpp_type_get_type");
        API(fieldOffset,"il2cpp_field_get_offset");
        API(elementClass,"il2cpp_class_get_element_class");
        API(objectHeaderSize,"il2cpp_object_header_size");
#undef API
        const auto& symbols=ownPlayerBindings();
        for(const auto& [key,symbol]:symbols.types) {
            api.classes[key]=resolveType(images,symbol);
        }
        for(const auto& [key,symbol]:symbols.fields) {
            auto field=resolveField(images,symbol);
            const auto expectedStorage=(key=="UGCTerrain.minstance" || key=="IdentityModules.Scene")?0x10u:0u;
            if((fieldFlags(field)&0x50)!=expectedStorage)
                throw std::runtime_error("Own-player field profile mismatch: "+key);
            const auto kind=typeKind(fieldType(field));
            const bool boolean=key=="BaseActor.isDestroyed";
            const bool identity=key=="BaseActor.IdentityID" || key=="IdentityScene.SelfID";
            if((boolean && kind!=2) || (identity && kind!=0xb) ||
               (!boolean && !identity && kind!=0x12 && kind!=0x15))
                throw std::runtime_error("Own-player field type mismatch: "+key);
            fields[key]=field;
        }
        bound=true;
    }
    std::vector<void*> actors(void* dictionary) {
        if(!dictionary) return {};
        const auto klass=api.object_get_class(dictionary);
        auto& layout=dictionaries[klass];
        auto field=[&](const char* primary,const char* fallback) {
            auto result=getField(klass,primary);
            return result?result:getField(klass,fallback);
        };
        if(!layout.entries) {
            layout.entries=field("_entries","entries");
            layout.count=field("_count","count");
            layout.version=field("_version","version");
        }
        if(!layout.entries || !layout.count || !layout.version) throw std::runtime_error("Actor dictionary metadata unavailable");
        if((fieldFlags(layout.entries)&0x50) || (fieldFlags(layout.count)&0x50) || (fieldFlags(layout.version)&0x50) ||
           typeKind(fieldType(layout.entries))!=0x1d || typeKind(fieldType(layout.count))!=8 || typeKind(fieldType(layout.version))!=8)
            throw std::runtime_error("Actor dictionary field types mismatch");
        const int count=read<int>(dictionary,layout.count),version=read<int>(dictionary,layout.version);
        if(count<0 || count>8192) throw std::runtime_error("Invalid actor dictionary count");
        auto array=api.hold(read<void*>(dictionary,layout.entries));
        if(!count) return {};
        if(!array || api.array_length(array)<size_t(count)) throw std::runtime_error("Invalid actor dictionary array");
        auto arrayClass=api.object_get_class(array);
        auto entryClass=arrayClass?elementClass(arrayClass):nullptr;
        if(!arrayClass || !entryClass) throw std::runtime_error("Actor dictionary entry class unavailable");
        const int stride=api.array_element_size(arrayClass);
        const auto header=objectHeaderSize();
        auto hashField=getField(entryClass,"hashCode"),nextField=getField(entryClass,"next"),valueField=getField(entryClass,"value");
        if(!hashField || !nextField || !valueField || stride<16 || stride>128)
            throw std::runtime_error("Actor dictionary entry metadata unavailable");
        if((fieldFlags(hashField)&0x50) || (fieldFlags(nextField)&0x50) || (fieldFlags(valueField)&0x50) ||
           typeKind(fieldType(nextField))!=8)
            throw std::runtime_error("Actor dictionary entry fields mismatch");
        auto hash=fieldOffset(hashField),next=fieldOffset(nextField),value=fieldOffset(valueField);
        if(hash<header || next<header || value<header || hash-header+4>size_t(stride) ||
           next-header+4>size_t(stride) || value-header+sizeof(void*)>size_t(stride))
            throw std::runtime_error("Actor dictionary entry layout mismatch");
        hash-=header;next-=header;value-=header;
        const int hashKind=typeKind(fieldType(hashField));
        if(hashKind!=8 && hashKind!=9) throw std::runtime_error("Actor dictionary hash type mismatch");
        std::vector<void*> result;
        const auto data=static_cast<const uint8_t*>(array)+api.array_object_header_size();
        for(int i=0;i<count;++i) {
            int32_t hashCode=0,nextIndex=0;void* actor=nullptr;
            const auto entry=data+size_t(i)*size_t(stride);
            std::memcpy(&hashCode,entry+hash,4);std::memcpy(&nextIndex,entry+next,4);
            std::memcpy(&actor,entry+value,sizeof(actor));
            if(nextIndex>=count) throw std::runtime_error("Actor dictionary chain is invalid");
            if(nextIndex>=-1 && (hashKind==9 || hashCode>=0) && actor) result.push_back(actor);
        }
        if(read<int>(dictionary,layout.version)!=version) return {};
        return result;
    }
    OwnPlayerState update() {
        if(!bound) throw std::runtime_error("Own-player metadata has not been bound");
        api.clearTemps();
        OwnPlayerState result;
        void* terrain=nullptr;
        readStatic(fields.at("UGCTerrain.minstance"),&terrain);api.hold(terrain);
        if(!is(terrain,"UGCTerrain")) return result;
        auto map=api.hold(read<void*>(terrain,"UGCTerrain.curMapData"));
        result.matchActive=map!=nullptr;
        if(!map) return result;
        void* scene=nullptr;
        readStatic(fields.at("IdentityModules.Scene"),&scene);api.hold(scene);
        if(!is(scene,"IdentityScene")) return result;
        const auto self=read<uint64_t>(scene,"IdentityScene.SelfID");
        auto dictionary=api.hold(read<void*>(terrain,"UGCTerrain.ActorDic"));
        for(auto actor:actors(dictionary)) {
            if(!is(actor,"UGCActor") || read<bool>(actor,"BaseActor.isDestroyed") ||
               read<uint64_t>(actor,"BaseActor.IdentityID")!=self) continue;
            auto character=api.hold(read<void*>(actor,"BaseActor.Character"));
            api.hold(actor);
            result.actor=reinterpret_cast<uintptr_t>(actor);
            result.character=reinterpret_cast<uintptr_t>(character);
            break;
        }
        return result;
    }
};
OwnPlayer::OwnPlayer():impl_(std::make_unique<Impl>()) {}
OwnPlayer::~OwnPlayer()=default;
void OwnPlayer::bind(const std::vector<void*>& images) { impl_->bind(images); }
OwnPlayerState OwnPlayer::update() { return impl_->update(); }
}
