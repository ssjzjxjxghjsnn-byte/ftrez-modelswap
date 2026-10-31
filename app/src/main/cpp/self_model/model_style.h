#pragma once
#include "self_model/model_format.h"
#include <algorithm>

namespace ft::self_model {


inline int modelStyle(const Model& model,int requested) {
    if(!model.rendererBytes.empty()) return 4;
    if(model.gltfMaterials) return 5;
    return requested==4 ? 2 : std::clamp(requested,0,3);
}
inline const char* styleName(int style) {
    return style==5?"GLB materials":style==0?"Realistic":style==1?"Realistic + outline":style==3?"Eevee Toon":style==4?"Custom renderer":"Toon";
}
inline bool alphaPart(const Part& part) { return (part.flags&2)!=0; }
inline bool outlinePart(const Part& part,int style) { return style!=0 && (part.flags&6)==0; }
inline std::array<float,4> materialColor(const Material& material,int hairTone) {

    constexpr float tones[4][3]{{1,1,1},{1,.9f,.8f},{.86f,.93f,1},{.62f,.62f,.7f}};
    auto color=material.color;
    if(material.name=="hair") for(size_t i=0;i<3;++i) color[i]*=tones[std::clamp(hairTone,0,3)][i];
    return color;
}
inline std::array<float,4> outlineColor(const Material& material) {
    std::array<float,4> color{0,0,0,1};
    for(size_t i=0;i<3;++i) color[i]=material.outlineRGB ?
        float((material.outlineRGB>>(i*8))&255)/255.f : std::clamp(material.color[i]*.22f,.015f,.24f);
    return color;
}

template<class Float,class Color,class Keyword,class Texture>
void applyStyle(int style,bool cloned,const Lighting& light,Float number,Color color,Keyword keyword,Texture texture) {
    if(cloned) {
        for(auto key:{"_Metallic","_MetallicFactor","_MetallicStrength","_MetallicScale"}) number(key,0.f);
        for(auto key:{"_Smoothness","_Glossiness","_SmoothnessFactor","_GlossMapScale"}) number(key,.3f);
    } else { number("_Smoothness",.15f); number("_Metallic",0.f); }
    number("_AlphaClip",0.f); number("_Cutoff",0.f); number("_Cull",2.f);
    if(style==2) {
        number("_ShadowRampWidth",light.shadowRamp); number("_ShadowOffset",light.shadowOffset);
        number("_specularStrength",0.f); number("_Shininess",0.f); number("_AlphaFactor",1.f);
        number("_Surface",0.f); number("_ZWrite",1.f); texture("_MaskTex");
    }
    number("_EmissionStrength",0.f); color("_EmissionColor",std::array<float,4>{0,0,0,1});
    if(style!=2 && light.rim>0) {
        keyword("_ENABLE_RIMLIGHT"); number("_ENABLE_RIMLIGHT",1.f);
        color("_RimColor",std::array<float,4>{1,.9f,.93f,1});
        number("_RimPower",3.5f); number("_RimIntensity",light.rim);
    }
}
}
