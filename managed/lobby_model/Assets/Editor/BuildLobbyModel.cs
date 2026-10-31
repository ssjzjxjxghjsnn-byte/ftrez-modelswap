using System;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

public static class BuildLobbyModel
{
    public static void Build()
    {
        if (EditorUserBuildSettings.activeBuildTarget != BuildTarget.Android)
            throw new Exception("Launch Unity with -buildTarget Android before building the lobby bundle");
        PlayerSettings.colorSpace = ColorSpace.Linear;
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { GraphicsDeviceType.OpenGLES3, GraphicsDeviceType.Vulkan });
        // Core's ShaderPreprocessor strips every URP pass when the build's
        // Graphics/Quality pipeline tag is empty, even without compiler errors.
        const string pipelinePath = "Assets/LobbyModel/BuildPipeline.asset";
        var pipeline = AssetDatabase.LoadAssetAtPath<UniversalRenderPipelineAsset>(pipelinePath);
        if (!pipeline)
        {
            var renderer = ScriptableObject.CreateInstance<UniversalRendererData>();
            AssetDatabase.CreateAsset(renderer, "Assets/LobbyModel/BuildRenderer.asset");
            pipeline = UniversalRenderPipelineAsset.Create(renderer);
            AssetDatabase.CreateAsset(pipeline, pipelinePath);
        }
        GraphicsSettings.defaultRenderPipeline = pipeline;
        int quality = QualitySettings.GetQualityLevel();
        for (int i = 0; i < QualitySettings.names.Length; ++i)
        {
            QualitySettings.SetQualityLevel(i, false);
            QualitySettings.renderPipeline = pipeline;
        }
        QualitySettings.SetQualityLevel(quality, false);
        var globalType = typeof(UniversalRenderPipelineAsset).Assembly.GetType(
            "UnityEngine.Rendering.Universal.UniversalRenderPipelineGlobalSettings", true);
        var ensure = globalType.GetMethod("Ensure", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
        var globals = (UnityEngine.Object)ensure.Invoke(null, new object[] { "", true });
        var settings = new SerializedObject(globals);
        settings.FindProperty("m_StripUnusedVariants").boolValue = false;
        settings.FindProperty("m_StripUnusedPostProcessingVariants").boolValue = false;
        settings.ApplyModifiedPropertiesWithoutUndo();
        EditorUtility.SetDirty(globals);
        AssetDatabase.SaveAssets();
        if (GraphicsSettings.currentRenderPipeline != pipeline || pipeline.renderPipelineShaderTag != "UniversalPipeline")
            throw new Exception("Lobby bundle requires the UniversalPipeline build tag");
        const string asset = "Assets/LobbyModel/LobbyModel.shader";
        var shader = AssetDatabase.LoadAssetAtPath<Shader>(asset);
        if (!shader || ShaderUtil.ShaderHasError(shader)) throw new Exception("Lobby shader failed to import");
        Directory.CreateDirectory("Build/Android");
        var result = BuildPipeline.BuildAssetBundles("Build/Android", new[] { new AssetBundleBuild {
            assetBundleName = "lobby-model.bundle", assetNames = new[] { asset }
        } }, BuildAssetBundleOptions.ChunkBasedCompression | BuildAssetBundleOptions.StrictMode |
             BuildAssetBundleOptions.ForceRebuildAssetBundle, BuildTarget.Android);
        if (!result || result.GetAllDependencies("lobby-model.bundle").Length != 0 || ShaderUtil.ShaderHasError(shader))
            throw new Exception("Lobby shader bundle failed validation");
        File.WriteAllText("Build/Android/pipeline.json", "{\"pipeline\":\"UniversalPipeline\",\"stripUnusedVariants\":false,\"graphicsAPIs\":[\"OpenGLES3\",\"Vulkan\"]}");
        Debug.Log("Lobby shader bundle built with UniversalPipeline for GLES3/Vulkan; binary validation required");
    }
}
