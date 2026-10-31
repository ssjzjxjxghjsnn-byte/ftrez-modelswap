using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using Object = UnityEngine.Object;

namespace FTRez.LobbyModel
{
    internal sealed class LobbyView : IDisposable
    {
        const BindingFlags instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        static readonly FieldInfo rendererDataList = RequireField(typeof(UniversalRenderPipelineAsset), "m_RendererDataList");
        static readonly FieldInfo rendererList = RequireField(typeof(UniversalRenderPipelineAsset), "m_Renderers");
        static readonly FieldInfo defaultRenderer = RequireField(typeof(UniversalRenderPipelineAsset), "m_DefaultRendererIndex");
        static readonly FieldInfo volumeComponents = RequireField(typeof(VolumeStack), "components");
        static readonly FieldInfo cameraVolume = RequireField(typeof(UniversalAdditionalCameraData), "m_VolumeStack");
        static readonly MethodInfo createRenderer = typeof(ScriptableRendererData).GetMethod("InternalCreateRenderer", instance);
        static readonly PropertyInfo currentRenderer = typeof(ScriptableRenderer).GetProperty("current", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        static readonly MethodInfo featureActive = typeof(UniversalAdditionalCameraData).GetMethod("IsRenderFeatureActive", new[] { typeof(ScriptableRendererFeature) });
        static readonly Type colorBuffers = typeof(UniversalRenderPipeline).Assembly.GetType("UnityEngine.Rendering.Universal.Internal.RenderTargetBufferSystem", true);
        static readonly FieldInfo bufferSide = colorBuffers.GetField("m_AisBackBuffer", BindingFlags.Static | BindingFlags.NonPublic);
        static readonly FieldInfo bufferDescriptor = colorBuffers.GetField("m_Desc", BindingFlags.Static | BindingFlags.NonPublic);
        UniversalRendererData rendererData;
        ScriptableRenderer renderer;
        VolumeStack volume;
        FieldValues pipeline;
        FieldValues restorePipeline;
        ShaderState shaderState;
        ShaderState restoreShaders;
        ScriptableRendererData[] expandedData;
        ScriptableRenderer[] expandedRenderers;
        readonly List<Material> ownedMaterials = new List<Material>();
        bool allowHDR, stopNaN, dithering, renderShadows, requiresOpaqueTexture;
        AntialiasingMode antialiasing;
        AntialiasingQuality antialiasingQuality;
        public bool PostProcessing { get; private set; }
        public int Samples { get; private set; }
        public GraphicsFormat ColorFormat { get; private set; }
        public bool HasFeatures { get; private set; }
        public string Description { get; private set; }
        bool disposed;
        LayerBindings layerBindings;

        static FieldInfo RequireField(Type type, string name)
        {
            var result = type.GetField(name, instance);
            if (result == null) throw new MissingFieldException(type.FullName, name);
            return result;
        }

        public static LobbyView Capture(Camera camera, ref RenderingData data)
        {
            var result = new LobbyView();
            try
            {
                var asset = UniversalRenderPipeline.asset;
                if (!asset || !(data.cameraData.renderer is UniversalRenderer))
                    throw new InvalidOperationException("Lobby does not use UniversalRenderer");
                var rendererArray = (ScriptableRenderer[])rendererList.GetValue(asset);
                var dataArray = (ScriptableRendererData[])rendererDataList.GetValue(asset);
                int index = Array.IndexOf(rendererArray, data.cameraData.renderer);
                if (index < 0) index = (int)defaultRenderer.GetValue(asset);
                var source = index >= 0 && index < dataArray.Length ? dataArray[index] as UniversalRendererData : null;
                if (!source || createRenderer == null) throw new InvalidOperationException("Lobby renderer data unavailable");
                result.rendererData = Object.Instantiate(source);
                result.rendererData.name = "FTREZ.Lobby.Renderer";
                result.rendererData.hideFlags = HideFlags.HideAndDontSave;
                var extra = camera.GetComponent<UniversalAdditionalCameraData>();
                // Retain the actual camera's feature order and effective enabled
                // state, with separate instances and materials for private use.
                result.rendererData.rendererFeatures.Clear();
                var existingMaterials = MaterialIds();
                try
                {
                    foreach (var sourceFeature in source.rendererFeatures)
                    {
                        if (!sourceFeature || sourceFeature is LobbyFeature) continue;
                        var copy = Object.Instantiate(sourceFeature);
                        copy.hideFlags = HideFlags.HideAndDontSave;
                        result.rendererData.rendererFeatures.Add(copy);
                        CloneMaterials(copy, result.ownedMaterials, new HashSet<object>());
                        bool active = sourceFeature.isActive && (featureActive == null || !extra ||
                            (bool)featureActive.Invoke(extra, new object[] { sourceFeature }));
                        copy.SetActive(active); result.HasFeatures |= active;
                    }
                }
                finally { result.RememberGeneratedMaterials(existingMaterials); }
                result.rendererData.intermediateTextureMode = IntermediateTextureMode.Always;
                result.pipeline = new FieldValues(asset);
                result.shaderState = new ShaderState();
                result.volume = VolumeManager.instance.CreateStack();
                var sourceComponents = (Dictionary<Type, VolumeComponent>)volumeComponents.GetValue(VolumeManager.instance.stack);
                var destination = (Dictionary<Type, VolumeComponent>)volumeComponents.GetValue(result.volume);
                foreach (var pair in sourceComponents)
                {
                    if (!pair.Value) continue;
                    var copy = Object.Instantiate(pair.Value);
                    copy.hideFlags = HideFlags.HideAndDontSave;
                    VolumeComponent previous;
                    if (destination.TryGetValue(pair.Key, out previous)) Entry.DestroyOwned(previous);
                    destination[pair.Key] = copy;
                }
                result.PostProcessing = data.cameraData.postProcessEnabled;
                result.allowHDR = data.cameraData.isHdrEnabled;
                result.requiresOpaqueTexture = data.cameraData.requiresOpaqueTexture;
                result.Samples = data.cameraData.cameraTargetDescriptor.msaaSamples;
                result.ColorFormat = data.cameraData.cameraTargetDescriptor.graphicsFormat;
                result.stopNaN = extra && extra.stopNaN;
                result.dithering = extra && extra.dithering;
                result.renderShadows = !extra || extra.renderShadows;
                result.antialiasing = data.cameraData.antialiasing;
                result.antialiasingQuality = data.cameraData.antialiasingQuality;
                result.Description = camera.name + " HDR=" + result.allowHDR + " post=" + result.PostProcessing +
                    " MSAA=" + result.Samples + " format=" + data.cameraData.cameraTargetDescriptor.graphicsFormat +
                    " features=" + string.Join(",", result.rendererData.rendererFeatures.ConvertAll(f =>
                        f.GetType().FullName + "=" + f.isActive));
                return result;
            }
            catch { result.Dispose(); throw; }
        }

        public IDisposable Apply(Camera camera, LayerIsolation isolation)
        {
            if (disposed) throw new ObjectDisposedException("LobbyView");
            return new Scope(this, camera, isolation);
        }

        public void LogDescription(int stage)
        {
            // Android truncates long log records; keep every view/feature visible.
            Debug.Log("[FTREZ-LOBBY] view=" + stage + " " + Description.Split(new[] { " features=" }, StringSplitOptions.None)[0]);
            foreach (var feature in rendererData.rendererFeatures)
                Debug.Log("[FTREZ-LOBBY] view=" + stage + " feature=" + feature.GetType().FullName + " active=" + feature.isActive);
        }

        sealed class Scope : IDisposable
        {
            readonly LobbyView view;
            readonly UniversalRenderPipelineAsset asset;
            readonly UniversalAdditionalCameraData cameraData;
            readonly FieldValues previousPipeline;
            readonly ScriptableRendererData[] previousData;
            readonly ScriptableRenderer[] previousRenderers;
            readonly VolumeStack previousVolume, previousCameraVolume;
            readonly object previousCurrent;
            readonly object previousBufferSide, previousBufferDescriptor;
            readonly ShaderState previousShaders;
            bool disposed;
            public Scope(LobbyView view, Camera camera, LayerIsolation isolation)
            {
                this.view = view; asset = UniversalRenderPipeline.asset;
                if (!asset) throw new InvalidOperationException("Match URP asset unavailable");
                cameraData = camera.GetComponent<UniversalAdditionalCameraData>();
                if (view.restorePipeline == null) view.restorePipeline = new FieldValues(asset);
                else view.restorePipeline.Capture(asset);
                previousPipeline = view.restorePipeline;
                previousData = (ScriptableRendererData[])rendererDataList.GetValue(asset);
                previousRenderers = (ScriptableRenderer[])rendererList.GetValue(asset);
                previousVolume = VolumeManager.instance.stack; previousCameraVolume = cameraData.volumeStack;
                previousCurrent = currentRenderer == null ? null : currentRenderer.GetValue(null);
                // These are static even though each renderer owns different
                // textures. A nested render between Base and Overlay must not
                // make the Overlay select/reallocate the wrong world buffer.
                if (bufferSide == null || bufferDescriptor == null)
                    throw new MissingFieldException("RenderTargetBufferSystem shared state");
                previousBufferSide = bufferSide.GetValue(null);
                previousBufferDescriptor = bufferDescriptor.GetValue(null);
                if (view.restoreShaders == null) view.restoreShaders = new ShaderState();
                else view.restoreShaders.Capture();
                previousShaders = view.restoreShaders;
                try
                {
                    view.pipeline.Apply(asset);
                    view.shaderState.Apply();
                    // Targets already use the actual world pixel dimensions.
                    // Applying renderScale again would scale this camera twice.
                    asset.renderScale = 1;
                    if (view.renderer == null)
                    {
                        // Renderer construction calls feature.Create again.
                        // Keep both generations of private pass materials even
                        // when the game feature replaces its pass without Dispose.
                        var existingMaterials = MaterialIds();
                        try { view.renderer = (ScriptableRenderer)createRenderer.Invoke(view.rendererData, null); }
                        finally { view.RememberGeneratedMaterials(existingMaterials); }
                        view.layerBindings = new LayerBindings(view.rendererData.rendererFeatures);
                    }
                    view.layerBindings.Apply(isolation);
                    // Built-in passes copy the renderer data masks at Create.
                    // LayerBindings includes the renderer's geometry pass filters.
                    if (view.rendererLayers == null) view.rendererLayers = new LayerBindings(new object[] { view.renderer });
                    view.rendererLayers.Apply(isolation);
                    if (view.expandedData == null || view.expandedData.Length != previousData.Length + 1)
                    {
                        view.expandedData = new ScriptableRendererData[previousData.Length + 1];
                        view.expandedRenderers = new ScriptableRenderer[view.expandedData.Length];
                    }
                    Array.Copy(previousData, view.expandedData, previousData.Length);
                    Array.Copy(previousRenderers, view.expandedRenderers, Math.Min(previousRenderers.Length, previousData.Length));
                    view.expandedData[previousData.Length] = view.rendererData;
                    view.expandedRenderers[previousData.Length] = view.renderer;
                    rendererDataList.SetValue(asset, view.expandedData); rendererList.SetValue(asset, view.expandedRenderers);
                    cameraData.SetRenderer(previousData.Length);
                    // The public setter pools an outgoing stack when set to
                    // null. This stack is owned by the snapshot, not that pool.
                    cameraVolume.SetValue(cameraData, view.volume); VolumeManager.instance.stack = view.volume;
                    camera.allowHDR = view.allowHDR;
                    camera.allowMSAA = view.Samples > 1;
                    cameraData.renderPostProcessing = view.PostProcessing;
                    cameraData.renderShadows = view.renderShadows;
                    cameraData.stopNaN = view.stopNaN; cameraData.dithering = view.dithering;
                    cameraData.antialiasing = view.antialiasing;
                    cameraData.antialiasingQuality = view.antialiasingQuality;
                    cameraData.requiresDepthTexture = true;
                    // Preserve the captured camera's resolved requirement. A
                    // feature can still request Color via ConfigureInput.
                    cameraData.requiresColorTexture = view.requiresOpaqueTexture;
                    cameraData.allowXRRendering = false;
                    // The game's GlobalRenderPostProcessing setter writes EVERY
                    // registered camera. The private instance flag is sufficient;
                    // never call that global setter, even temporarily.
                }
                catch { Dispose(); throw; }
            }
            public void Dispose()
            {
                if (disposed) return; disposed = true;
                cameraVolume.SetValue(cameraData, previousCameraVolume);
                VolumeManager.instance.stack = previousVolume;
                rendererDataList.SetValue(asset, previousData); rendererList.SetValue(asset, previousRenderers);
                cameraData.SetRenderer(-1);
                previousPipeline.Apply(asset);
                if (currentRenderer != null && currentRenderer.GetSetMethod(true) != null) currentRenderer.SetValue(null, previousCurrent);
                bufferSide.SetValue(null, previousBufferSide);
                bufferDescriptor.SetValue(null, previousBufferDescriptor);
                previousShaders.Apply();
            }
        }

        LayerBindings rendererLayers;
        // Preserve each feature's original per-layer selection while assigning
        // private draw aliases. Some game features copy LayerMask into a pass's
        // FilteringSettings in Create, others read their settings each frame.
        sealed class LayerBindings
        {
            sealed class Binding
            {
                public object Root, Original;
                public FieldInfo[] Path;
                public void Apply(LayerIsolation layers)
                {
                    object value;
                    if (Original is LayerMask mask) value = (LayerMask)layers.Map(mask.value);
                    else { var filter = (FilteringSettings)Original; filter.layerMask = layers.Map(filter.layerMask); value = filter; }
                    Set(Root, 0, value);
                }
                void Set(object owner, int index, object value)
                {
                    var field = Path[index];
                    if (index == Path.Length - 1) { field.SetValue(owner, value); return; }
                    var child = field.GetValue(owner);
                    SetChild(child, index + 1, value);
                    if (field.FieldType.IsValueType) field.SetValue(owner, child);
                }
                void SetChild(object owner, int index, object value) { Set(owner, index, value); }
            }
            readonly List<Binding> bindings = new List<Binding>();
            public LayerBindings(IEnumerable roots)
            {
                foreach (var root in roots) Visit(root, root, new List<FieldInfo>(), new HashSet<object>(), 0);
            }
            void Visit(object root, object value, List<FieldInfo> path, HashSet<object> visited, int depth)
            {
                if (value == null || depth > 8 || !visited.Add(value)) return;
                var valueType = value.GetType();
                if (valueType.IsPrimitive || valueType.IsEnum || value is string || value is Delegate) return;
                if (value is Object && !(value is ScriptableRendererFeature)) return;
                for (var type = valueType; type != null && type != typeof(Object) && type != typeof(ScriptableObject) &&
                    type != typeof(ScriptableRenderer) && type != typeof(ScriptableRenderPass); type = type.BaseType)
                    foreach (var field in type.GetFields(instance | BindingFlags.DeclaredOnly))
                    {
                        if (field.IsStatic) continue;
                        var child = field.GetValue(value);
                        path.Add(field);
                        if (!field.IsInitOnly && (child is LayerMask || child is FilteringSettings))
                            bindings.Add(new Binding { Root = root, Path = path.ToArray(), Original = child });
                        else if (!(child is IList)) Visit(root, child, path, visited, depth + 1);
                        path.RemoveAt(path.Count - 1);
                    }
            }
            public void Apply(LayerIsolation isolation) { foreach (var binding in bindings) binding.Apply(isolation); }
        }

        static void CloneMaterials(object value, List<Material> owned, HashSet<object> visited)
        {
            if (value == null || !visited.Add(value)) return;
            for (var type = value.GetType(); type != null && type != typeof(ScriptableObject) && type != typeof(Object); type = type.BaseType)
                foreach (var field in type.GetFields(instance | BindingFlags.DeclaredOnly))
                {
                    if (field.IsStatic || field.IsInitOnly || field.IsNotSerialized ||
                        (!field.IsPublic && !field.IsDefined(typeof(SerializeField), true))) continue;
                    var child = field.GetValue(value);
                    if (child is Material material)
                    {
                        var copy = new Material(material) { hideFlags = HideFlags.HideAndDontSave };
                        owned.Add(copy); field.SetValue(value, copy);
                    }
                    else if (child is IList list)
                    {
                        for (int i = 0; i < list.Count; ++i)
                            if (list[i] is Material item)
                            {
                                var copy = new Material(item) { hideFlags = HideFlags.HideAndDontSave };
                                owned.Add(copy); list[i] = copy;
                            }
                            else if (!(list[i] is Object)) CloneMaterials(list[i], owned, visited);
                    }
                    else if (child != null && !(child is Object) && !child.GetType().IsValueType && !(child is string))
                        CloneMaterials(child, owned, visited);
                }
        }

        static HashSet<int> MaterialIds()
        {
            var result = new HashSet<int>();
            foreach (var material in Resources.FindObjectsOfTypeAll<Material>())
                if (material) result.Add(material.GetInstanceID());
            return result;
        }

        void RememberGeneratedMaterials(HashSet<int> existing)
        {
            if (!rendererData) return;
            var visited = new HashSet<object>();
            foreach (var feature in rendererData.rendererFeatures)
                RememberGeneratedMaterials(feature, existing, visited, 0);
        }

        void RememberGeneratedMaterials(object value, HashSet<int> existing, HashSet<object> visited, int depth)
        {
            if (value == null || depth > 12) return;
            if (value is Material material)
            {
                // Only resources born during this synchronous clone/Create
                // operation can become ours. Existing shared assets stay owned
                // by the game, including nonserialized pass materials.
                if (material && !existing.Contains(material.GetInstanceID()) && !ownedMaterials.Contains(material))
                    ownedMaterials.Add(material);
                return;
            }
            var valueType = value.GetType();
            if (valueType.IsPrimitive || valueType.IsEnum || value is string || value is Delegate ||
                value is ScriptableRenderer || (value is Object && !(value is ScriptableRendererFeature)) ||
                !visited.Add(value)) return;
            if (value is IList list)
            {
                foreach (var child in list) RememberGeneratedMaterials(child, existing, visited, depth + 1);
                return;
            }
            // Inspect feature/settings/custom pass fields, never the renderer,
            // scene objects or the engine's internal base-pass graph.
            for (var type = valueType; type != null && type != typeof(ScriptableObject) &&
                type != typeof(Object) && type != typeof(ScriptableRenderPass); type = type.BaseType)
                foreach (var field in type.GetFields(instance | BindingFlags.DeclaredOnly))
                    if (!field.IsStatic)
                        RememberGeneratedMaterials(field.GetValue(value), existing, visited, depth + 1);
        }

        // Copy serialized scalar settings by name/type, never version-specific
        // offsets. This includes the game's extra shadow quality switches.
        sealed class FieldValues
        {
            static readonly FieldInfo[] fields = Fields();
            readonly object[] values = new object[fields.Length];
            static FieldInfo[] Fields()
            {
                var selected = new List<FieldInfo>();
                foreach (var field in typeof(UniversalRenderPipelineAsset).GetFields(instance))
                {
                    var type = field.FieldType;
                    if (field.IsInitOnly || field.IsStatic || field.Name == "m_DefaultRendererIndex" ||
                        field.Name.StartsWith("k_") || field.Name == "m_EnableRenderGraph" ||
                        !(field.IsPublic || field.IsDefined(typeof(SerializeField), true))) continue;
                    if (!type.IsPrimitive && !type.IsEnum && type != typeof(Vector2) && type != typeof(Vector3) && type != typeof(Vector4)) continue;
                    selected.Add(field);
                }
                return selected.ToArray();
            }
            public FieldValues(UniversalRenderPipelineAsset asset) { Capture(asset); }
            public void Capture(UniversalRenderPipelineAsset asset)
            {
                for (int i = 0; i < fields.Length; ++i) values[i] = fields[i].GetValue(asset);
            }
            public void Apply(UniversalRenderPipelineAsset asset)
            {
                for (int i = 0; i < fields.Length; ++i) fields[i].SetValue(asset, values[i]);
            }
        }

        public void Dispose()
        {
            if (disposed) return; disposed = true;
            bool rendererOwnedFeatures = renderer != null;
            if (renderer != null) { Entry.RemoveFeature(renderer); renderer.Dispose(); renderer = null; }
            if (rendererData)
            {
                foreach (var feature in rendererData.rendererFeatures) if (feature)
                {
                    // Instantiate invokes OnEnable/Create even when this
                    // snapshot is replaced before its first private render.
                    if (!rendererOwnedFeatures) feature.Dispose();
                    Entry.DestroyOwned(feature);
                }
                Entry.DestroyOwned(rendererData);
            }
            foreach (var material in ownedMaterials) Entry.DestroyOwned(material);
            ownedMaterials.Clear();
            if (volume != null) { VolumeManager.instance.DestroyStack(volume); volume = null; }
        }
    }
}
