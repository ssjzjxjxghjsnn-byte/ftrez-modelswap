#pragma once
#include "self_model/model_texture_decode.h"

namespace ft::self_model {
struct ModelTextureUpload {
    void *ctor=nullptr,*setPixels=nullptr,*apply=nullptr;
    template<class Api> void bind(Api& api) {
        ctor=setPixels=apply=nullptr;
        auto color=api.classes.find("UnityEngine.Color32"); uint32_t alignment=0;
        if(color==api.classes.end() || !color->second || api.class_value_size(color->second,&alignment)!=sizeof(ModelPixel)) return;
        ctor=api.method("UnityEngine.Texture2D",".ctor",{"System.Int32","System.Int32","UnityEngine.TextureFormat","System.Boolean","System.Boolean"},"System.Void",false,true);
        setPixels=api.method("UnityEngine.Texture2D","SetPixels32",{"UnityEngine.Color32[]"},"System.Void",false,true);
        apply=api.method("UnityEngine.Texture2D","Apply",{"System.Boolean","System.Boolean"},"System.Void",false,true);
    }
    bool ready() const { return ctor && setPixels && apply; }
    template<class Api,class Create> void* upload(Api& api,Create create,const DecodedModelTexture& decoded,
                                                 const TextureSampling& sampling={}) const {
        int width=decoded.width,height=decoded.height,format=4;
        bool mipChain=sampling.mipmaps,linear=sampling.linear,unreadable=true;
        auto texture=create(ctor,{&width,&height,&format,&mipChain,&linear});
        api.call(setPixels,texture,{api.template array<ModelPixel>("UnityEngine.Color32",decoded.pixels.get(),decoded.count())});

        api.call(apply,texture,{&mipChain,&unreadable});
        return texture;
    }
};
}
