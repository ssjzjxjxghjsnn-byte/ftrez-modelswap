#ifndef FTREZ_GLTF_INCLUDED
#define FTREZ_GLTF_INCLUDED
#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"
#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Shadows.hlsl"

TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap);
TEXTURE2D(_MetallicRoughnessMap); SAMPLER(sampler_MetallicRoughnessMap);
TEXTURE2D(_NormalMap); SAMPLER(sampler_NormalMap);
TEXTURE2D(_OcclusionMap); SAMPLER(sampler_OcclusionMap);
TEXTURE2D(_EmissionMap); SAMPLER(sampler_EmissionMap);
CBUFFER_START(UnityPerMaterial)
float4 _BaseColor, _EmissionColor;
float _Metallic, _Roughness, _NormalScale, _OcclusionStrength;
float _AlphaMode, _Cutoff, _Unlit, _HasNormalMap;
float _Cull, _SrcBlend, _DstBlend, _ZWrite;
CBUFFER_END
float3 _LightDirection, _LightPosition;
struct Attributes {
    float4 positionOS : POSITION;
    float3 normalOS : NORMAL;
    float2 uv : TEXCOORD0;
    UNITY_VERTEX_INPUT_INSTANCE_ID
};
struct Varyings {
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD0;
    half3 normalWS : TEXCOORD1;
    float2 uv : TEXCOORD2;
    half fog : TEXCOORD3;
    UNITY_VERTEX_INPUT_INSTANCE_ID
    UNITY_VERTEX_OUTPUT_STEREO
};
Varyings GLTFVertex(Attributes input) {
    Varyings output=(Varyings)0;
    UNITY_SETUP_INSTANCE_ID(input);
    UNITY_TRANSFER_INSTANCE_ID(input,output);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);
    VertexPositionInputs position=GetVertexPositionInputs(input.positionOS.xyz);
    output.positionCS=position.positionCS; output.positionWS=position.positionWS;
    output.normalWS=TransformObjectToWorldNormal(input.normalOS);
    output.uv=input.uv; output.fog=ComputeFogFactor(position.positionCS.z);
    return output;
}
Varyings GLTFShadowVertex(Attributes input) {
    Varyings output=GLTFVertex(input);
    float3 direction=_LightDirection;
    #if defined(_CASTING_PUNCTUAL_LIGHT_SHADOW)
        direction=normalize(_LightPosition-output.positionWS);
    #endif
    output.positionCS=TransformWorldToHClip(ApplyShadowBias(output.positionWS,output.normalWS,direction));
    #if UNITY_REVERSED_Z
        output.positionCS.z=min(output.positionCS.z,UNITY_NEAR_CLIP_VALUE*output.positionCS.w);
    #else
        output.positionCS.z=max(output.positionCS.z,UNITY_NEAR_CLIP_VALUE*output.positionCS.w);
    #endif
    return output;
}
half4 GLTFBase(float2 uv) {
    // sRGB Texture2D performs the conversion on sampling in a Linear project.
    // Do not apply pow/gamma to glTF's already-linear factors or to alpha.
    half4 sample=SAMPLE_TEXTURE2D(_BaseMap,sampler_BaseMap,uv);
    return sample*_BaseColor;
}
void GLTFClip(half alpha) { if(_AlphaMode>0.5 && _AlphaMode<1.5) clip(alpha-_Cutoff); }
half3 GLTFNormal(Varyings input,half facing) {
    half3 n=normalize(input.normalWS)*facing;
    if(_HasNormalMap<0.5) return n;
    half3 normalTS=SAMPLE_TEXTURE2D(_NormalMap,sampler_NormalMap,input.uv).xyz*2-1;
    normalTS.xy*=_NormalScale;
    // Decoder flips V for Unity. Reconstruct glTF's original UV basis so the
    // normal map's green channel keeps its original direction, including mirrors.
    float2 uv=float2(input.uv.x,1-input.uv.y);
    float2 a=ddx(uv),b=ddy(uv);
    float determinant=a.x*b.y-a.y*b.x;
    if(abs(determinant)<1e-8) return n;
    float3 dx=ddx(input.positionWS),dy=ddy(input.positionWS);
    float3 t=(dx*b.y-dy*a.y)/determinant;
    float3 bitangent=(dy*a.x-dx*b.x)/determinant;
    t-=n*dot(n,t); bitangent-=n*dot(n,bitangent);
    if(dot(t,t)<1e-12 || dot(bitangent,bitangent)<1e-12) return n;
    return normalize(normalize(t)*normalTS.x+normalize(bitangent)*normalTS.y+n*normalTS.z);
}
half4 GLTFFragment(Varyings input,FRONT_FACE_TYPE face : FRONT_FACE_SEMANTIC) : SV_Target {
    UNITY_SETUP_INSTANCE_ID(input);
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);
    half4 base=GLTFBase(input.uv); GLTFClip(base.a);
    half3 emission=SAMPLE_TEXTURE2D(_EmissionMap,sampler_EmissionMap,input.uv).rgb*_EmissionColor.rgb;
    half3 output;
    if(_Unlit>0.5) output=base.rgb; // KHR_materials_unlit ignores lighting and emission
    else {
        half4 mr=SAMPLE_TEXTURE2D(_MetallicRoughnessMap,sampler_MetallicRoughnessMap,input.uv);
        SurfaceData surface=(SurfaceData)0;
        surface.albedo=base.rgb;
        surface.alpha=1;
        surface.metallic=saturate(mr.b*_Metallic);
        surface.smoothness=1-saturate(mr.g*_Roughness);
        surface.occlusion=lerp(1,SAMPLE_TEXTURE2D(_OcclusionMap,sampler_OcclusionMap,input.uv).r,_OcclusionStrength);
        surface.emission=emission;
        surface.normalTS=half3(0,0,1);
        InputData data=(InputData)0;
        data.positionWS=input.positionWS;
        data.normalWS=GLTFNormal(input,IS_FRONT_VFACE(face,1,-1));
        data.viewDirectionWS=GetWorldSpaceNormalizeViewDir(input.positionWS);
        data.shadowCoord=TransformWorldToShadowCoord(input.positionWS);
        data.fogCoord=input.fog;
        data.vertexLighting=VertexLighting(input.positionWS,data.normalWS);
        data.bakedGI=SampleSH(data.normalWS);
        data.normalizedScreenSpaceUV=GetNormalizedScreenSpaceUV(input.positionCS);
        data.shadowMask=half4(1,1,1,1);
        output=UniversalFragmentPBR(data,surface).rgb;
    }
    return half4(MixFog(output,input.fog),_AlphaMode>1.5?base.a:1);
}
half4 GLTFDepth(Varyings input) : SV_Target {
    UNITY_SETUP_INSTANCE_ID(input);
    if(_AlphaMode>1.5) clip(-1); // blended surfaces do not write a solid depth/shadow silhouette
    GLTFClip(GLTFBase(input.uv).a); return 0;
}
half4 GLTFNormals(Varyings input,FRONT_FACE_TYPE face : FRONT_FACE_SEMANTIC) : SV_Target {
    GLTFDepth(input);
    half3 n=GLTFNormal(input,IS_FRONT_VFACE(face,1,-1));
    #if defined(_GBUFFER_NORMALS_OCT)
        float2 oct=PackNormalOctQuadEncode(n);
        return half4(PackFloat2To888(saturate(oct*.5+.5)),0);
    #else
        return half4(n,0);
    #endif
}
#endif
