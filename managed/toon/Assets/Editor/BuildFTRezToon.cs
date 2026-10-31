using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

public static class BuildFTRezToon
{


    public static void Configure()
    {

        PlayerSettings.colorSpace = ColorSpace.Linear;
        const string pipelinePath = "Assets/FTRezToon/BuildPipeline.asset";
        var pipeline = AssetDatabase.LoadAssetAtPath<UniversalRenderPipelineAsset>(pipelinePath);
        if (pipeline == null)
        {
            var renderer = ScriptableObject.CreateInstance<UniversalRendererData>();
            AssetDatabase.CreateAsset(renderer, "Assets/FTRezToon/BuildRenderer.asset");
            pipeline = UniversalRenderPipelineAsset.Create(renderer);
            AssetDatabase.CreateAsset(pipeline, pipelinePath);
        }
        var so = new SerializedObject(pipeline);
        foreach (var name in new[] { "m_MainLightShadowsSupported", "m_AdditionalLightShadowsSupported", "m_SoftShadowsSupported" })
            so.FindProperty(name).boolValue = true;
        so.FindProperty("m_AdditionalLightsRenderingMode").intValue = 1;
        so.ApplyModifiedPropertiesWithoutUndo();
        GraphicsSettings.defaultRenderPipeline = pipeline;
        QualitySettings.renderPipeline = pipeline;
        var globalType = typeof(UniversalRenderPipelineAsset).Assembly.GetType("UnityEngine.Rendering.Universal.UniversalRenderPipelineGlobalSettings", true);
        var ensure = globalType.GetMethod("Ensure", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
        var globals = (UnityEngine.Object)ensure.Invoke(null, new object[] { "", true });
        var globalSO = new SerializedObject(globals);
        globalSO.FindProperty("m_StripUnusedVariants").boolValue = false;
        globalSO.FindProperty("m_StripDebugVariants").boolValue = true;
        globalSO.ApplyModifiedPropertiesWithoutUndo();
        EditorUtility.SetDirty(globals);
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { GraphicsDeviceType.OpenGLES3, GraphicsDeviceType.Vulkan });
        AssetDatabase.SaveAssets();
    }

    public static void Build()
    {
        Configure();
        if (!BuildPipeline.IsBuildTargetSupported(BuildTargetGroup.Android, BuildTarget.Android))
            throw new InvalidOperationException("Install Android Build Support first.");
        if (!EditorUserBuildSettings.SwitchActiveBuildTarget(BuildTargetGroup.Android, BuildTarget.Android))
            throw new InvalidOperationException("Cannot activate Android build target.");
        if (GraphicsSettings.currentRenderPipeline == null ||
            !GraphicsSettings.currentRenderPipeline.GetType().FullName.Contains("UniversalRenderPipeline"))
            throw new InvalidOperationException("Configure the matching URP pipeline before building.");
        const string asset = "Assets/FTRezToon/SimpleURPToonLitOutlineExample.shader";
        AssetDatabase.Refresh();
        var shader = AssetDatabase.LoadAssetAtPath<Shader>(asset);
        if (shader == null || ShaderUtil.ShaderHasError(shader))
            throw new InvalidOperationException("Toon shader import failed; inspect Unity shader errors.");

        var names = new[] { asset, "Assets/FTRezToon/opaque.mat", "Assets/FTRezToon/clipped.mat" };
        for (int i = 1; i < names.Length; ++i)
        {
            var material = AssetDatabase.LoadAssetAtPath<Material>(names[i]);
            if (material == null) { material = new Material(shader); AssetDatabase.CreateAsset(material, names[i]); }
            material.shader = shader;
            if (i == 2) material.EnableKeyword("_UseAlphaClipping");
            else material.DisableKeyword("_UseAlphaClipping");
            EditorUtility.SetDirty(material);
        }
        AssetDatabase.SaveAssets();
        const string gltfAsset = "Assets/FTRezGLTF/GLTF.shader";
        var gltfShader = AssetDatabase.LoadAssetAtPath<Shader>(gltfAsset);
        if (gltfShader == null || ShaderUtil.ShaderHasError(gltfShader))
            throw new InvalidOperationException("GLTF shader import failed.");
        names = names.Concat(new[] { gltfAsset }).ToArray();
        var output = Path.GetFullPath("Build/Android");
        Directory.CreateDirectory(output);
        var result = BuildPipeline.BuildAssetBundles(output,
            new[] { new AssetBundleBuild { assetBundleName = "ftrez-toon.bundle", assetNames = names } },
            BuildAssetBundleOptions.ChunkBasedCompression | BuildAssetBundleOptions.StrictMode | BuildAssetBundleOptions.ForceRebuildAssetBundle,
            BuildTarget.Android);
        if (result == null || ShaderUtil.ShaderHasError(shader) || ShaderUtil.ShaderHasError(gltfShader))
            throw new InvalidOperationException("Android toon bundle build failed.");
        var built = AssetBundle.LoadFromFile(Path.Combine(output, "ftrez-toon.bundle"));
        if (built == null) throw new InvalidOperationException("Bundle cannot be loaded.");
        try
        {
            var loaded = built.LoadAsset<Shader>(asset.ToLowerInvariant());
            if (loaded == null || loaded.name != "FTREZ/EeveeToon")
                throw new InvalidOperationException("Bundle shader lookup failed.");
            Debug.Log("Verified bundle shader lookup: " + loaded.name);
            if (built.LoadAsset<Shader>(gltfAsset.ToLowerInvariant())?.name != "FTREZ/GLTF")
                throw new InvalidOperationException("Bundle GLTF shader lookup failed.");
        }
        finally { built.Unload(true); }
        File.WriteAllLines(Path.Combine(output, "build-info.txt"), new[] {
            "Shader=FTREZ/EeveeToon", "URP=14.0.12", "GameURPPatch=unverified",
            "Unity=" + Application.unityVersion,
            "ColorSpace=" + PlayerSettings.colorSpace,
            "Pipeline=" + GraphicsSettings.currentRenderPipeline.GetType().FullName,
            "GraphicsAPIs=" + string.Join(",", PlayerSettings.GetGraphicsAPIs(BuildTarget.Android).Select(x => x.ToString()))
        });
        Debug.Log("FTREZ toon bundle: " + output);
    }
}
