#pragma once
#include "self_model/model_format.h"

namespace ft::self_model {


struct ModelTransformAccess {
    void* localToWorldOut=nullptr;
    void* worldToLocalOut=nullptr;
    void* positionAndRotation=nullptr;

    template<class Api> void bind(Api& api) {
        localToWorldOut=worldToLocalOut=positionAndRotation=nullptr;
        uint32_t alignment=0;
        if(api.class_value_size(api.klass("UnityEngine.Matrix4x4"),&alignment)==sizeof(Matrix)) {
            localToWorldOut=api.method("UnityEngine.Transform","get_localToWorldMatrix_Injected",
                {"UnityEngine.Matrix4x4&"},"System.Void",false,true);
            worldToLocalOut=api.method("UnityEngine.Transform","get_worldToLocalMatrix_Injected",
                {"UnityEngine.Matrix4x4&"},"System.Void",false,true);
        }
        positionAndRotation=api.method("UnityEngine.Transform","SetLocalPositionAndRotation",
            {"UnityEngine.Vector3","UnityEngine.Quaternion"},"System.Void",false,true);
    }
    template<class Api> Matrix read(Api& api,void* transform,void* boxedGetter,bool inverse=false) const {
        const auto injected=inverse?worldToLocalOut:localToWorldOut;
        if(!injected) return api.template value<Matrix>(boxedGetter,transform);
        Matrix result;
        api.call(injected,transform,{&result});
        return result;
    }
    template<class Api> void write(Api& api,void* transform,void* positionSetter,void* rotationSetter,
                                  const Vec3& position,const Quat& rotation) const {

        auto p=position; auto q=rotation;
        if(positionAndRotation) api.call(positionAndRotation,transform,{&p,&q});
        else {
            api.call(positionSetter,transform,{&p});
            api.call(rotationSetter,transform,{&q});
        }
    }
};
}
