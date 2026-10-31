Shader "Hidden/FTRez/LobbyModel"
{
    Properties
    {
        _MainTex ("Source", 2D) = "black" {}
        _LobbyColor ("Lobby model", 2D) = "black" {}
        _LobbyCoverage ("Model coverage/depth", 2D) = "black" {}
        _LobbyForeground ("Foreground depth", 2D) = "white" {}
        _LobbyDepthSampling ("MSAA depth footprint", Float) = 0
        _LobbyViewport ("Model viewport", Vector) = (0,0,1,1)
    }
    SubShader
    {
        Tags { "RenderPipeline"="UniversalPipeline" }
        Cull Off ZWrite Off ZTest Always
        HLSLINCLUDE
        #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
        TEXTURE2D(_MainTex); SAMPLER(sampler_MainTex);
        TEXTURE2D(_LobbyColor); SAMPLER(sampler_LobbyColor);
        TEXTURE2D(_LobbyCoverage); SAMPLER(sampler_LobbyCoverage);
        TEXTURE2D(_LobbyForeground); SAMPLER(sampler_LobbyForeground);
        TEXTURE2D_X_FLOAT(_CameraDepthTexture); SAMPLER(sampler_CameraDepthTexture);
        float4 _MainTex_TexelSize;
        float _LobbyDepthSampling;
        float4 _LobbyViewport;
        struct Attributes { float4 positionOS : POSITION; float2 uv : TEXCOORD0; };
        struct Varyings { float4 positionCS : SV_POSITION; float2 uv : TEXCOORD0; float2 depthUV : TEXCOORD1; };
        Varyings Vert(Attributes input)
        {
            Varyings output;
            output.positionCS = TransformObjectToHClip(input.positionOS.xyz);
            output.uv = input.uv;
            output.depthUV = input.uv;
            #if UNITY_UV_STARTS_AT_TOP
            if (_MainTex_TexelSize.y < 0) output.depthUV.y = 1 - output.depthUV.y;
            #endif
            return output;
        }
        float EyeDepth(float raw)
        {
            if (unity_OrthoParams.w < .5) return LinearEyeDepth(raw, _ZBufferParams);
            #if UNITY_REVERSED_Z
            raw = 1 - raw;
            #endif
            return lerp(_ProjectionParams.y, _ProjectionParams.z, raw);
        }
        float4 Coverage(Varyings input) : SV_Target
        {
            float depth = SAMPLE_TEXTURE2D_X(_CameraDepthTexture, sampler_CameraDepthTexture, input.depthUV).r;
            // Opaque shaders may output texture alpha without enabling blending
            // (Saki does). That alpha is not geometric coverage. The private
            // depth already respects cutouts and excludes non-depth-writing
            // transparent edges; URP's MSAA depth copy resolves the far sample.
            #if UNITY_REVERSED_Z
            float covered = step(0.000001, depth);
            #else
            float covered = 1 - step(.999999, depth);
            #endif
            return float4(covered, depth, 0, 0);
        }
        half4 Composite(Varyings input) : SV_Target
        {
            float2 modelUV = (input.depthUV - _LobbyViewport.xy) / _LobbyViewport.zw;
            float inViewport = step(0, modelUV.x) * step(0, modelUV.y) * step(modelUV.x, 1) * step(modelUV.y, 1);
            float2 coverage = SAMPLE_TEXTURE2D(_LobbyCoverage, sampler_LobbyCoverage, modelUV).rg;
            float modelZ = EyeDepth(coverage.y);
            // A resolved MSAA depth represents an offset sample, while a
            // single-sample world depth is at the pixel center. Allow that
            // sample footprint only on continuous, fully covered geometry.
            float slope = abs(ddx(modelZ)) + abs(ddy(modelZ));
            float continuous = step(fwidth(coverage.x), .0001) * step(slope, modelZ * .02);
            // Evaluate derivatives before any discard; uncovered pixels need
            // no world/foreground/color fetches.
            clip(step(.999, coverage.x) * inViewport - .5);
            float depth = SAMPLE_TEXTURE2D_X(_CameraDepthTexture, sampler_CameraDepthTexture, input.depthUV).r;
            float worldZ = EyeDepth(depth);
            float tolerance = max(.002, worldZ * .00001) + _LobbyDepthSampling * continuous * slope * .75;
            float visible = step(modelZ, worldZ + tolerance);
            float foregroundZ = SAMPLE_TEXTURE2D(_LobbyForeground, sampler_LobbyForeground, input.depthUV).r;
            visible *= step(modelZ - tolerance, foregroundZ);
            #if UNITY_REVERSED_Z
            visible *= step(0.000001, coverage.y);
            #else
            visible *= 1 - step(.999999, coverage.y);
            #endif
            // The original model is still in the game's color/depth buffers.
            // Replace covered opaque pixels; retain its normal blending at
            // translucent edges rather than blend a premultiplied image twice.
            // All coverage decisions are binary. Keep rejected framebuffer
            // pixels with discard and preserve its alpha via ColorMask RGB;
            // no full-world color copy or framebuffer sampling is required.
            clip(step(.999, coverage.x) * visible * inViewport - .5);
            half3 model = SAMPLE_TEXTURE2D(_LobbyColor, sampler_LobbyColor, modelUV).rgb;
            return half4(model, 1);
        }
        struct DepthVaryings { float4 positionCS : SV_POSITION; float eyeDepth : TEXCOORD0; };
        DepthVaryings DepthVert(Attributes input)
        {
            DepthVaryings output;
            output.positionCS = TransformObjectToHClip(input.positionOS.xyz);
            output.eyeDepth = -TransformWorldToView(TransformObjectToWorld(input.positionOS.xyz)).z;
            return output;
        }
        float DepthFrag(DepthVaryings input) : SV_Target { return input.eyeDepth; }
        ENDHLSL
        Pass
        {
            Name "CoverageDepth"
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex Vert
            #pragma fragment Coverage
            ENDHLSL
        }
        Pass
        {
            Name "SelfBeforeUI"
            ColorMask RGB
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex Vert
            #pragma fragment Composite
            ENDHLSL
        }
        Pass
        {
            Name "TransparentForegroundDepth"
            ZWrite On ZTest LEqual
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex DepthVert
            #pragma fragment DepthFrag
            ENDHLSL
        }
        Pass
        {
            Name "ReflectionSnapshot"
            HLSLPROGRAM
            #pragma target 3.5
            #pragma vertex SnapshotVert
            #pragma fragment SnapshotFrag
            TEXTURECUBE(_SnapshotCube); SAMPLER(sampler_SnapshotCube);
            float4 _SnapshotDecode, _SnapshotFaceMip;
            struct SnapshotVaryings { float4 positionCS : SV_POSITION; float2 uv : TEXCOORD0; };
            SnapshotVaryings SnapshotVert(uint id : SV_VertexID)
            {
                SnapshotVaryings o;
                o.uv = float2((id << 1) & 2, id & 2);
                o.positionCS = float4(o.uv * 2 - 1, 0, 1);
                #if UNITY_UV_STARTS_AT_TOP
                o.uv.y = 1 - o.uv.y;
                #endif
                return o;
            }
            float4 SnapshotFrag(SnapshotVaryings input) : SV_Target
            {
                float2 p = input.uv * 2 - 1;
                int face = (int)_SnapshotFaceMip.x;
                float3 d;
                if (face == 0) d = float3(1, -p.y, -p.x);
                else if (face == 1) d = float3(-1, -p.y, p.x);
                else if (face == 2) d = float3(p.x, 1, p.y);
                else if (face == 3) d = float3(p.x, -1, -p.y);
                else if (face == 4) d = float3(p.x, -p.y, 1);
                else d = float3(-p.x, -p.y, -1);
                float4 encoded = SAMPLE_TEXTURECUBE_LOD(_SnapshotCube, sampler_SnapshotCube, d, _SnapshotFaceMip.y);
                float alpha = max(_SnapshotDecode.w * (encoded.a - 1) + 1, 0);
                return float4(encoded.rgb * (_SnapshotDecode.x * pow(alpha, _SnapshotDecode.y)), 1);
            }
            ENDHLSL
        }
    }
    Fallback Off
}
