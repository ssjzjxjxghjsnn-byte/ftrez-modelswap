#pragma once
#include "self_model/model_format.h"

namespace ft::self_model {
// glTF factors are Linear already. Upload vectors verbatim, color textures as
// sRGB and data textures as Linear. Alpha is never gamma converted.
template<class Number,class Vector,class Texture,class Queue>
void applyGltfMaterial(const Material& material,Number number,Vector vector,Texture texture,Queue queue) {
    const auto& m=material.gltf;
    vector("_BaseColor",material.color);
    vector("_EmissionColor",std::array<float,4>{m.emission[0],m.emission[1],m.emission[2],1});
    number("_Metallic",m.metallic); number("_Roughness",m.roughness);
    number("_NormalScale",m.normalScale); number("_OcclusionStrength",m.occlusionStrength);
    number("_Unlit",m.unlit?1:0); number("_AlphaMode",float(m.alphaMode));
    number("_Cutoff",m.alphaCutoff); number("_Cull",material.flags&materialDoubleSided?0:2);
    number("_SrcBlend",m.alphaMode==2?5:1); number("_DstBlend",m.alphaMode==2?10:0);
    number("_ZWrite",m.alphaMode==2?0:1);
    texture("_MetallicRoughnessMap",m.metallicRoughness);
    texture("_NormalMap",m.normal); number("_HasNormalMap",m.normal>=0?1:0);
    texture("_OcclusionMap",m.occlusion); texture("_EmissionMap",m.emissive);
    queue(m.alphaMode==2?3000:m.alphaMode==1?2450:2000);
}
}
