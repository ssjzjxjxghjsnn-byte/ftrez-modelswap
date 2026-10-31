#pragma once
#include "self_model/unity_api.h"
#include "self_model/model_view.h"
#include "sdk/model_bindings.h"

namespace ft::self_model {


struct GameModelBinding {
    void *actorClass=nullptr,*cloth=nullptr,*clothOwner=nullptr,*first=nullptr,*arms=nullptr,*shown=nullptr;
    void *profileClass=nullptr,*binderField=nullptr,*mainBone=nullptr,*profileSelf=nullptr;
    void* (*class_get_field_from_name)(void*,const char*)=nullptr;
    void* (*field_get_type)(void*)=nullptr;
    uint32_t (*field_get_flags)(void*)=nullptr;
    void (*field_get_value)(void*,void*,void*)=nullptr;
    void bind(UnityApi& api,const std::vector<void*>& images,const sdk::Symbols& symbols) {
        if(!symbols.unresolved.empty()) throw std::runtime_error("Model binding profile incomplete");
        for(const auto& [key,s]:symbols.types) {
            api.classes[key]=sdk::resolveType(images,s);
        }
        auto fullName=[&](const char* key) {
            const auto& s=symbols.types.at(key);
            return s.namespc.empty()?s.type:s.namespc+"."+s.type;
        };
        actorClass=api.classes.at("BaseActor");
        profileClass=api.classes.at("ProfileUI");
        const auto& boneMethod=symbols.methods.at("TimelineBinder.MainBone");
        if(boneMethod.isStatic || boneMethod.result!="UnityEngine.Transform" ||
           boneMethod.parameters!=std::vector<std::string>{"System.String"})
            throw std::runtime_error("Configured main bone method signature mismatch");
        mainBone=sdk::resolveMethod(images,boneMethod);
#define MODEL_API(n) n=reinterpret_cast<decltype(n)>(unity::resolve_api("il2cpp_" #n))
        MODEL_API(class_get_field_from_name); MODEL_API(field_get_type);
        MODEL_API(field_get_flags); MODEL_API(field_get_value);
#undef MODEL_API
        if(!class_get_field_from_name || !field_get_type || !field_get_flags || !field_get_value)
            throw std::runtime_error("Model field API unavailable");
        auto field=[&](const char* owner,const char* key,const std::string& type) {
            const auto& s=symbols.fields.at(std::string(owner)+"."+key);
            auto f=class_get_field_from_name(api.classes.at(owner),s.member.c_str());
            if(!f || (field_get_flags(f)&0x50) || api.typeName(field_get_type(f))!=type)
                throw std::runtime_error(std::string("Model field mismatch: ")+key);
            return f;
        };
        cloth=field("ClothOwner","ClothState",fullName("ClothUpdater"));
        const auto& ownerSymbol=symbols.fields.at("BaseActor.ClothOwner");
        clothOwner=class_get_field_from_name(actorClass,ownerSymbol.member.c_str());
        if(!clothOwner || (field_get_flags(clothOwner)&0x50)) throw std::runtime_error("Cloth owner mismatch");
        first=field("ClothUpdater","First","System.Boolean");
        arms=field("ClothUpdater","Arms","System.Boolean");
        shown=field("ClothUpdater","Shown","System.Boolean");
        binderField=field("BaseUI","Binder",fullName("TimelineBinder"));
        profileSelf=field("ProfileUI","IsSelf","System.Boolean");
        auto k=profileClass;
        for(size_t depth=0;k && k!=api.classes.at("BaseUI") && depth<64;++depth) k=api.class_get_parent(k);
        if(k!=api.classes.at("BaseUI")) throw std::runtime_error("Profile base type mismatch");
    }
    std::optional<PlayerView> view(UnityApi& api,void* actor) {
        if(!actor || !actorClass || !cloth || !first || !arms || !shown) return {};
        auto k=api.object_get_class(actor);
        for(size_t depth=0;k && k!=actorClass && depth<64;++depth) k=api.class_get_parent(k);
        if(k!=actorClass) return {};
        void* updater=nullptr;
        void* owner=nullptr;
        field_get_value(actor,clothOwner,&owner); api.hold(owner);
        if(!owner) return {};
        auto ownerType=api.object_get_class(owner);
        for(size_t depth=0;ownerType && ownerType!=api.classes.at("ClothOwner") && depth<64;++depth)
            ownerType=api.class_get_parent(ownerType);
        if(ownerType!=api.classes.at("ClothOwner")) return {};
        field_get_value(owner,cloth,&updater);
        api.hold(updater);
        if(!updater) return {};
        PlayerView result;
        field_get_value(updater,first,&result.firstPerson);
        field_get_value(updater,arms,&result.showArms);
        field_get_value(updater,shown,&result.visible);
        return result;
    }
    bool ownProfile(void* profile) const {
        if(!profile || !profileSelf || !field_get_value) return false;


        bool own=false; field_get_value(profile,profileSelf,&own); return own;
    }
    void* previewBone(UnityApi& api,void* profile) {
        if(!ownProfile(profile) || !binderField || !mainBone) return nullptr;
        void* binder=nullptr;
        field_get_value(profile,binderField,&binder); api.hold(binder);
        return binder?api.call(mainBone,binder,{api.text("Bip001 Pelvis")}):nullptr;
    }
};
}
