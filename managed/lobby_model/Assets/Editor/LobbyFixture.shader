Shader "Hidden/FTRez/LobbyFixtureTint"
{
    Properties
    {
        _MainTex ("Source", 2D) = "black" {}
        _Tint ("Tint", Color) = (1, 1, 1, 1)
    }
    SubShader
    {
        Tags { "RenderPipeline" = "UniversalPipeline" }
        Pass
        {
            ZTest Always ZWrite Off Cull Off
            HLSLPROGRAM
            #pragma vertex vert_img
            #pragma fragment frag
            #include "UnityCG.cginc"
            sampler2D _MainTex;
            float4 _Tint;
            float4 frag(v2f_img input) : SV_Target
            {
                float4 source = tex2D(_MainTex, input.uv);
                return float4(source.rgb * _Tint.rgb, source.a);
            }
            ENDHLSL
        }
        Pass
        {
            ZTest LEqual ZWrite Off Cull Back
            Blend DstColor Zero
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            float4 _Tint;
            float4 vert(float4 vertex : POSITION) : SV_POSITION { return UnityObjectToClipPos(vertex); }
            float4 frag() : SV_Target { return float4(_Tint.rgb, 1); }
            ENDHLSL
        }
    }
}
