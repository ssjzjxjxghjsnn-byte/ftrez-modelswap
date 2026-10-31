Shader "FTREZ/GLTF"
{
    Properties
    {
        [MainTexture] _BaseMap("Base color (sRGB)",2D)="white" {}
        [MainColor] _BaseColor("Base factor (Linear)",Vector)=(1,1,1,1)
        _MetallicRoughnessMap("Metal B / roughness G (Linear)",2D)="white" {}
        _NormalMap("Normal (Linear)",2D)="bump" {}
        _OcclusionMap("Occlusion R (Linear)",2D)="white" {}
        _EmissionMap("Emission (sRGB)",2D)="white" {}
        _EmissionColor("Emission factor (Linear)",Vector)=(0,0,0,1)
        _Metallic("Metallic",Range(0,1))=1
        _Roughness("Roughness",Range(0,1))=1
        _NormalScale("Normal scale",Float)=1
        _OcclusionStrength("Occlusion strength",Range(0,1))=1
        _HasNormalMap("Normal map enabled",Float)=0
        _Unlit("KHR unlit",Float)=0
        _AlphaMode("Opaque / mask / blend",Float)=0
        _Cutoff("Alpha cutoff",Range(0,1))=.5
        [Enum(UnityEngine.Rendering.CullMode)] _Cull("Cull",Float)=2
        [HideInInspector] _SrcBlend("Source blend",Float)=1
        [HideInInspector] _DstBlend("Destination blend",Float)=0
        [HideInInspector] _ZWrite("Depth write",Float)=1
    }
    SubShader
    {
        Tags { "RenderPipeline"="UniversalPipeline" "RenderType"="Opaque" "Queue"="Geometry" }
        Cull [_Cull]
        HLSLINCLUDE
        #include "GLTF.hlsl"
        ENDHLSL
        Pass
        {
            Name "GLTFForward"
            Tags { "LightMode"="UniversalForwardOnly" }
            Blend [_SrcBlend] [_DstBlend]
            ZWrite [_ZWrite]
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex GLTFVertex
            #pragma fragment GLTFFragment
            #pragma multi_compile _ _MAIN_LIGHT_SHADOWS _MAIN_LIGHT_SHADOWS_CASCADE _MAIN_LIGHT_SHADOWS_SCREEN
            #pragma multi_compile _ _ADDITIONAL_LIGHTS_VERTEX _ADDITIONAL_LIGHTS
            #pragma multi_compile _ _FORWARD_PLUS
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_BLENDING
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_BOX_PROJECTION
            #pragma multi_compile_fragment _ _ADDITIONAL_LIGHT_SHADOWS
            #pragma multi_compile_fragment _ _SHADOWS_SOFT
            #pragma multi_compile_fragment _ _SCREEN_SPACE_OCCLUSION
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            ENDHLSL
        }
        Pass
        {
            Name "GLTFShadow"
            Tags { "LightMode"="ShadowCaster" }
            ZWrite On
            ZTest LEqual
            ColorMask 0
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex GLTFShadowVertex
            #pragma fragment GLTFDepth
            #pragma multi_compile_vertex _ _CASTING_PUNCTUAL_LIGHT_SHADOW
            #pragma multi_compile_instancing
            ENDHLSL
        }
        Pass
        {
            Name "GLTFDepth"
            Tags { "LightMode"="DepthOnly" }
            ZWrite On
            ColorMask R
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex GLTFVertex
            #pragma fragment GLTFDepth
            #pragma multi_compile_instancing
            ENDHLSL
        }
        Pass
        {
            Name "GLTFNormals"
            Tags { "LightMode"="DepthNormalsOnly" }
            ZWrite On
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex GLTFVertex
            #pragma fragment GLTFNormals
            #pragma multi_compile_fragment _ _GBUFFER_NORMALS_OCT
            #pragma multi_compile_instancing
            ENDHLSL
        }
    }
    Fallback Off
}
