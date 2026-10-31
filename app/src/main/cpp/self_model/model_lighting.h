#pragma once
#include "self_model/unity_api.h"
#include "self_model/model_reference.h"

namespace ft::self_model {



struct ModelLightingBinding {
    void* anchor=nullptr;
    void* frameCount=nullptr;
    void* getEnabled=nullptr;
    void* setEnabled=nullptr;
    void (*setField)(void*,void*,void*)=nullptr;
    void (*getField)(void*,void*,void*)=nullptr;
    std::string error;

    bool ready() const { return anchor && frameCount && getEnabled && setEnabled && setField && getField; }
    void writeAnchor(void* probe,void* transform) const {


        writeModelReference(setField,getField,probe,anchor,transform);
    }
    void bind(UnityApi& api,const std::vector<void*>& images) {
        try {
            void* found=nullptr;
            for(auto image:images) if(auto k=api.class_from_name(image,"","CustomProbeAgent")) {
                if(found && found!=k) throw std::runtime_error("Ambiguous CustomProbeAgent");
                found=k;
            }
            if(!found) throw std::runtime_error("CustomProbeAgent unavailable");
            api.classes["CustomProbeAgent"]=found;
            auto field=reinterpret_cast<void*(*)(void*,const char*)>(unity::resolve_api("il2cpp_class_get_field_from_name"));
            auto type=reinterpret_cast<void*(*)(void*)>(unity::resolve_api("il2cpp_field_get_type"));
            auto flags=reinterpret_cast<uint32_t(*)(void*)>(unity::resolve_api("il2cpp_field_get_flags"));
            setField=reinterpret_cast<decltype(setField)>(unity::resolve_api("il2cpp_field_set_value"));
            getField=reinterpret_cast<decltype(getField)>(unity::resolve_api("il2cpp_field_get_value"));
            if(!field || !type || !flags || !setField || !getField) throw std::runtime_error("Probe field API unavailable");
            auto f=field(found,"probeAnchor");
            if(!f || (flags(f)&0x10) || api.typeName(type(f))!="UnityEngine.Transform")
                throw std::runtime_error("Probe anchor signature changed");
            frameCount=api.method("CustomProbeAgent","SetFrameCount",{"System.Int32"},"System.Void");
            getEnabled=api.method("CustomProbeAgent","get_enabled",{},"System.Boolean");
            setEnabled=api.method("CustomProbeAgent","set_enabled",{"System.Boolean"},"System.Void");
            anchor=f;
        } catch(const std::exception& e) { anchor=nullptr; error=e.what(); }
    }
};
}
