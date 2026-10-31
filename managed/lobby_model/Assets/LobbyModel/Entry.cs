using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using Object = UnityEngine.Object;

namespace FTRez.LobbyModel
{
    // Runs inside the game's camera pipeline. It never creates a screen Canvas
    // or writes to the display after UI has rendered.
    public static partial class Entry
    {
        public const string Revision = "lobby-direct-20261023";
        public static void SetGeometryHooks(IntPtr state) { GeometryChanges.NativeState = state; }
        public static void WatchInstantiation(AsyncOperation operation) { GeometryChanges.Watch(operation); }
        sealed class Target
        {
            public Transform Host;
            public Renderer[] Renderers = Array.Empty<Renderer>();
            public RenderTexture ColorA, ColorB, Coverage;
            public Material Composite;
            public bool Ready;
            public int Samples;
            public OriginalModel Original;
            public Rect Viewport = new Rect(0, 0, 1, 1);
            public int FullWidth, FullHeight;
            public readonly CropSizing Sizing = new CropSizing();
            public readonly List<SpareBuffers> Spare = new List<SpareBuffers>(2);
        }
        sealed class SpareBuffers
        {
            public RenderTexture ColorA, ColorB, Coverage;
            public long Bytes;
        }
        static readonly Target[] targets = { new Target(), new Target(), new Target() };
        static Transform lobbySource;
        static Renderer[] captureRenderers = Array.Empty<Renderer>();
        static Target activeTarget;
        static bool enabled, initialized, renderingPrivate, beforeUI;
        static Camera lobbyCamera, worldCamera, privateCamera;
        static UniversalAdditionalCameraData privateData;
        static readonly Dictionary<ScriptableRenderer, LobbyFeature> features =
            new Dictionary<ScriptableRenderer, LobbyFeature>();
        static readonly List<Camera> lobbyMembers = new List<Camera>();
        static readonly List<Camera> uiCameras = new List<Camera>();
        static LobbyEnvironment environment, pendingEnvironment;
        static readonly List<LobbyView> views = new List<LobbyView>();
        static readonly List<LobbyView> pendingViews = new List<LobbyView>();
        static RenderTexture foreground;
        static Material coverageMaterial;
        static Shader ownedShader;
        static readonly string[] requiredPasses = { "CoverageDepth", "SelfBeforeUI", "TransparentForegroundDepth", "ReflectionSnapshot" };
        internal static Shader SnapshotShader { get { return coverageMaterial ? coverageMaterial.shader : null; } }
        static RenderTexture seed;
        static int captureFrame = -1, coverageFrame = -1;
        static int composedFrame = -1, captureStage = -1, privateStage;
        static float nextCapture, nextUICheck;
        static float lastCompositeTime = -1;
        static int baselineGeneration;
        static bool loggedProbes;
        static string lastFailure, cameraFailure;
        static double privateMilliseconds;
        static int privateCalls, privateStages;
        static float nextTiming;
        static LayerIsolation isolation;
        static int isolationFrame = -1;
        static readonly Plane[] modelFrustum = new Plane[6];
        static readonly Vector3[] cropCorners = new Vector3[8];
        static readonly Vector3[] cropProjected = new Vector3[8];
        static string message = "Lobby rendering: waiting for lobby";
        static readonly List<Material> foregroundMaterials = new List<Material>();
        static string activeMessage, activeFailure;
        static int activeCount = -1, activeGeneration = -1;
        static readonly PropertyInfo rendererFeatures = typeof(ScriptableRenderer).GetProperty(
            "rendererFeatures", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        static readonly MethodInfo setFeatureActive = typeof(UniversalAdditionalCameraData).GetMethod(
            "SetRenderFeatureActive", new[] { typeof(Type), typeof(bool) });

        public static void Initialize(byte[] bundleBytes)
        {
            if (initialized) return;
            AssetBundle bundle;
            try { bundle = AssetBundle.LoadFromMemory(bundleBytes); }
            catch (Exception error) { throw new InvalidOperationException("Lobby init [BUNDLE_LOAD]: " + error.Message, error); }
            if (!bundle) throw new InvalidOperationException("Lobby init [BUNDLE_LOAD]: bundle could not load");
            bool success = false;
            try
            {
                var shader = bundle.LoadAsset<Shader>("assets/lobbymodel/lobbymodel.shader");
                if (!shader) throw new InvalidOperationException("Lobby init [ASSET_MISSING]: LobbyModel.shader");
                CheckShader(shader);
                ShaderProbe.Verify(shader);
                InitializeShader(shader);
                ownedShader = shader;
                success = true;
            }
            finally { bundle.Unload(!success); }
        }

        static void CheckShader(Shader shader)
        {
            if (!shader) throw new InvalidOperationException("Lobby init [ASSET_MISSING]: shader is null");
            if (shader.passCount != requiredPasses.Length)
                throw new InvalidOperationException("Lobby init [PASS_MISSING]: expected " + requiredPasses.Length + ", found " + shader.passCount);
            if (!shader.isSupported)
                throw new InvalidOperationException("Lobby init [SHADER_UNSUPPORTED]: " + SystemInfo.graphicsDeviceType);
        }

        internal static void InitializeShader(Shader shader)
        {
            if (initialized) return;
            if (rendererFeatures == null) throw new MissingMemberException("Lobby init [SETUP]: ScriptableRenderer.rendererFeatures");
            CheckShader(shader);
            try
            {
                coverageMaterial = new Material(shader) { hideFlags = HideFlags.HideAndDontSave };
                for (int i = 0; i < requiredPasses.Length; ++i)
                    if (coverageMaterial.FindPass(requiredPasses[i]) != i)
                        throw new InvalidOperationException("Lobby init [PASS_MISSING]: " + requiredPasses[i]);
                RenderPipelineManager.beginCameraRendering += BeginCamera;
                RenderPipelineManager.endCameraRendering += EndCamera;
                if (!DirectRendering) GeometryChanges.Install();
                initialized = true;
            }
            catch
            {
                RenderPipelineManager.beginCameraRendering -= BeginCamera;
                RenderPipelineManager.endCameraRendering -= EndCamera;
                DestroyOwned(coverageMaterial); coverageMaterial = null;
                initialized = false;
                throw;
            }
            Debug.Log("[FTREZ-LOBBY] " + Revision + " initialized; Unity=" + Application.unityVersion +
                " graphics=" + SystemInfo.graphicsDeviceType + " colorSpace=" + QualitySettings.activeColorSpace);
        }

        public static string GetStatus()
        {
            if (DirectRendering)
            {
                if (enabled && message.Contains("active on") && Time.realtimeSinceStartup - lastCompositeTime > 2)
                    Report("Lobby lighting: baseline ready; waiting for model camera");
                return message + " | " + Revision;
            }
            if (enabled && message.Contains("active on") && Time.realtimeSinceStartup - lastCompositeTime > 2)
                Report("Lobby rendering: baseline ready; no recent model composition");
            return message + " | " + Revision;
        }

        static void Failure(string stage, Exception error)
        {
            string detail = stage + ": " + error;
            if (lastFailure != detail) Debug.LogWarning("[FTREZ-LOBBY] " + Revision + " " + detail);
            lastFailure = detail;
            Report("Lobby rendering: retained; " + stage + ": " + error.GetBaseException().Message);
        }

        static void Report(string text)
        {
            if (message == text) return;
            message = text;
            Debug.Log("[FTREZ-LOBBY] " + text);
        }

        public static void SetTarget(Transform target, Renderer[] renderers, bool match)
        {
            SetTargets(match ? null : target, new[] { target, null, null }, renderers,
                new int[renderers == null ? 0 : renderers.Length], true);
        }

        // Stable slots: Self, Rabbit, own Profile. A flat renderer array avoids
        // dependence on AOT-generated jagged-array interop in HybridCLR.
        public static void SetTargets(Transform source, Transform[] hosts, Renderer[] renderers, int[] owners, bool useLobby)
        {
            // Legacy Editor fixtures. The native bridge supplies the camera
            // read from the IL-bound Lobby UI, not a hierarchy heuristic.
            SetTargets(source, source ? source.root.GetComponentInChildren<Camera>() : null, hosts, renderers, owners, useLobby);
        }

        public static void SetTargets(Transform source, Camera sourceCamera, Transform[] hosts, Renderer[] renderers, int[] owners, bool useLobby)
        {
            bool sourceChanged = lobbySource != source || lobbyCamera != sourceCamera;
            bool changed = sourceChanged || enabled != useLobby;
            if (hosts == null || hosts.Length != targets.Length ||
                (renderers == null ? 0 : renderers.Length) != (owners == null ? 0 : owners.Length))
                throw new ArgumentException("Invalid lobby model target set");
            if (DirectRendering) RestoreDirectFrames();
            var grouped = new List<Renderer>[targets.Length];
            for (int i = 0; i < grouped.Length; ++i) grouped[i] = new List<Renderer>();
            if (renderers != null) for (int i = 0; i < renderers.Length; ++i)
            {
                if (owners[i] < 0 || owners[i] >= targets.Length) throw new ArgumentException("Invalid lobby model owner");
                if (renderers[i] && !grouped[owners[i]].Contains(renderers[i])) grouped[owners[i]].Add(renderers[i]);
            }
            enabled = useLobby; lobbySource = useLobby ? source : null;
            bool any = false;
            for (int i = 0; i < targets.Length; ++i)
            {
                var target = targets[i];
                changed |= target.Host != hosts[i] || target.Renderers.Length != grouped[i].Count;
                if (target.Renderers.Length == grouped[i].Count)
                    for (int j = 0; j < grouped[i].Count; ++j) changed |= target.Renderers[j] != grouped[i][j];
                target.Host = useLobby ? hosts[i] : null;
                target.Renderers = target.Host ? grouped[i].ToArray() : Array.Empty<Renderer>();
                target.Ready = false;
                any |= target.Host && target.Renderers.Length != 0;
                if (!target.Host || target.Renderers.Length == 0) ReleaseProduct(target);
            }
            worldCamera = null;
            if (changed) { nextCapture = 0; nextUICheck = 0; }
            if (sourceChanged) DiscardPending();
            if (DirectRendering)
            {
                SetDirectTargets(sourceCamera, any, changed, sourceChanged);
                return;
            }
            if (!enabled || (!lobbySource && !any))
            {
                lobbyCamera = null;
                DiscardPending();
                ReleasePrivate();
                Report(enabled ? "Lobby rendering: waiting for models" : "Lobby rendering: off");
                return;
            }
            if (lobbySource)
            {
                lobbyCamera = sourceCamera;
                captureRenderers = targets[0].Host == lobbySource && targets[0].Renderers.Length != 0 ?
                    targets[0].Renderers : lobbySource.GetComponentsInChildren<Renderer>(true);
                if (!lobbyCamera || !lobbyCamera.isActiveAndEnabled) Report("Lobby rendering: waiting for active lobby camera");
                else if (sourceChanged) Report("Lobby rendering: waiting for complete lobby capture");
            }
            else
            {
                lobbyCamera = null;
                if (environment == null || !environment.Ready || views.Count == 0)
                    Report("Lobby rendering: waiting for a captured lobby");
                else if (changed) Report("Lobby rendering: baseline ready; waiting for model camera");
            }
        }

        static bool Visible(Renderer renderer)
        {
            return renderer && renderer.enabled && renderer.gameObject.activeInHierarchy && !renderer.forceRenderingOff;
        }

        static bool HasOwnLayer(Camera camera)
        {
            foreach (var target in targets) if (target.Host)
                foreach (var renderer in target.Renderers)
                    if (Visible(renderer) && (camera.cullingMask & (1 << renderer.gameObject.layer)) != 0) return true;
            return false;
        }

        static bool VisibleIn(Target target, Camera camera, Plane[] frustum)
        {
            if (!target.Host || !target.Host.gameObject.activeInHierarchy) return false;
            foreach (var renderer in target.Renderers)
                if (Visible(renderer) && (camera.cullingMask & (1 << renderer.gameObject.layer)) != 0 &&
                    GeometryUtility.TestPlanesAABB(frustum, renderer.bounds)) return true;
            return false;
        }

        static bool IsWorldCamera(Camera camera)
        {
            if (!camera || camera.cameraType != CameraType.Game || !camera.enabled)
                return false;
            if (camera.rect != new Rect(0, 0, 1, 1) || !HasOwnLayer(camera)) return false;
            if (Time.realtimeSinceStartup >= nextUICheck)
            {
                uiCameras.Clear();
                foreach (var canvas in Resources.FindObjectsOfTypeAll<Canvas>())
                    if (canvas && canvas.renderMode == RenderMode.ScreenSpaceCamera && canvas.worldCamera &&
                        !uiCameras.Contains(canvas.worldCamera)) uiCameras.Add(canvas.worldCamera);
                nextUICheck = Time.realtimeSinceStartup + .5f;
            }
            var ownData = camera.GetComponent<UniversalAdditionalCameraData>();
            beforeUI = uiCameras.Contains(camera);
            // Profile meshes can be drawn by the UI camera itself. Replace its
            // opaque model before transparents/UI, while that camera has no post.
            if (beforeUI)
            {
                if (ownData && ownData.renderPostProcessing)
                { Report("Lobby rendering: original retained; UI camera also post-processes its meshes"); return false; }
                return true;
            }
            Camera main = camera;
            if (ownData && ownData.renderType == CameraRenderType.Overlay)
                foreach (var candidate in Camera.allCameras)
                {
                    var candidateData = candidate.GetComponent<UniversalAdditionalCameraData>();
                    if (candidateData && candidateData.renderType == CameraRenderType.Base &&
                        candidateData.cameraStack != null && candidateData.cameraStack.Contains(camera))
                    { main = candidate; break; }
                }
            var data = main.GetComponent<UniversalAdditionalCameraData>();
            if (!data || data.renderType != CameraRenderType.Base) return true;
            var stack = data.cameraStack;
            Camera lastWorld = main;
            bool reachedUI = false;
            if (stack != null) foreach (var member in stack)
            {
                if (!member || !member.isActiveAndEnabled) continue;
                var extra = member.GetComponent<UniversalAdditionalCameraData>();
                if (!extra || extra.renderType != CameraRenderType.Overlay) continue;
                reachedUI |= uiCameras.Contains(member);
                if (reachedUI)
                {
                    if (!uiCameras.Contains(member) || extra.renderPostProcessing)
                    {
                        Report("Lobby rendering: original retained; processing after UI/world boundary");
                        return false;
                    }
                }
                else if (HasOwnLayer(member)) lastWorld = member;
            }
            // Compose once, after the final world camera has applied its post.
            return camera == lastWorld;
        }

        internal static void EnsureFeature(ScriptableRenderer renderer, UniversalAdditionalCameraData data)
        {
            if (renderer == null) throw new InvalidOperationException("Camera renderer is unavailable");
            LobbyFeature feature;
            if (!features.TryGetValue(renderer, out feature) || !feature)
            {
                feature = ScriptableObject.CreateInstance<LobbyFeature>();
                feature.name = "FTREZ.LobbyModel";
                feature.hideFlags = HideFlags.HideAndDontSave;
                feature.Create();
                features[renderer] = feature;
            }
            var list = rendererFeatures.GetValue(renderer) as List<ScriptableRendererFeature>;
            if (list == null) throw new InvalidOperationException("Camera feature list is unavailable");
            if (!list.Contains(feature)) list.Add(feature);
            feature.SetActive(true);
            // The game extends URP with a per-camera disabled-feature list.
            // The Editor version has no such method.
            if (data && setFeatureActive != null)
                setFeatureActive.Invoke(data, new object[] { typeof(LobbyFeature), true });
        }

        internal static void RemoveFeature(ScriptableRenderer renderer)
        {
            LobbyFeature feature;
            if (!features.TryGetValue(renderer, out feature)) return;
            var list = rendererFeatures.GetValue(renderer) as List<ScriptableRendererFeature>;
            if (list != null) list.Remove(feature);
            features.Remove(renderer);
            if (feature) { feature.Dispose(); DestroyOwned(feature); }
        }

        internal static void DestroyOwned(Object value)
        {
            if (!value) return;
            if (Application.isPlaying) Object.Destroy(value); else Object.DestroyImmediate(value);
        }

        static void ReleasePrivate()
        {
            worldCamera = null;
            if (privateCamera) privateCamera.targetTexture = null;
            foreach (var target in targets) ReleaseProduct(target);
            ReleaseTarget(ref foreground);
            if (privateCamera) DestroyOwned(privateCamera.gameObject);
            privateCamera = null; privateData = null;
            foreach (var renderer in new List<ScriptableRenderer>(features.Keys)) RemoveFeature(renderer);
        }

        public static void Shutdown()
        {
            ShutdownDirect();
            RenderPipelineManager.beginCameraRendering -= BeginCamera;
            RenderPipelineManager.endCameraRendering -= EndCamera;
            ReleasePrivate(); DiscardPending();
            foreach (var view in views) view.Dispose(); views.Clear();
            if (environment != null) environment.Dispose(); environment = null;
            DestroyOwned(coverageMaterial); coverageMaterial = null;
            DestroyOwned(ownedShader); ownedShader = null;
            foreach (var target in targets) { target.Host = null; target.Renderers = Array.Empty<Renderer>(); }
            lobbySource = null; captureRenderers = Array.Empty<Renderer>(); lobbyCamera = null;
            initialized = renderingPrivate = enabled = false;
            activeTarget = null; nextCapture = nextUICheck = 0;
            isolation = null; isolationFrame = -1;
            privateMilliseconds = 0; privateCalls = privateStages = 0; nextTiming = 0;
            lastCompositeTime = -1; baselineGeneration = 0; lastFailure = cameraFailure = null;
            foregroundMaterials.Clear(); activeMessage = activeFailure = null;
            activeCount = activeGeneration = -1;
            loggedProbes = false;
        }

        static void BeginCamera(ScriptableRenderContext context, Camera camera)
        {
            if (renderingPrivate || !initialized || !enabled) return;
            if (DirectRendering) { BeginDirectCamera(camera); return; }
            try
            {
                if (lobbyCamera)
                {
                    if (camera == lobbyCamera && Time.realtimeSinceStartup >= nextCapture)
                    {
                        DiscardPending();
                        captureFrame = Time.frameCount;
                        lobbyMembers.Add(camera);
                        var baseData = camera.GetComponent<UniversalAdditionalCameraData>();
                        if (baseData && baseData.renderType == CameraRenderType.Base && baseData.cameraStack != null)
                            foreach (var member in baseData.cameraStack)
                            {
                                var data = member ? member.GetComponent<UniversalAdditionalCameraData>() : null;
                                if (member && member.isActiveAndEnabled && data && data.renderType == CameraRenderType.Overlay)
                                    lobbyMembers.Add(member);
                            }
                    }
                    captureStage = captureFrame == Time.frameCount ? lobbyMembers.IndexOf(camera) : -1;
                    if (captureStage >= 0)
                    {
                        var data = camera.GetComponent<UniversalAdditionalCameraData>();
                        EnsureFeature(data ? data.scriptableRenderer : UniversalRenderPipeline.asset.scriptableRenderer, data);
                    }
                    if (camera == lobbyCamera || captureStage >= 0) return;
                }
                if (environment == null || !environment.Ready || views.Count == 0 || !IsWorldCamera(camera)) return;
                var cameraData = camera.GetComponent<UniversalAdditionalCameraData>();
                var renderer = cameraData ? cameraData.scriptableRenderer : UniversalRenderPipeline.asset.scriptableRenderer;
                if (!(renderer is UniversalRenderer))
                { Report("Lobby rendering: retained; model camera renderer is not UniversalRenderer"); return; }
                EnsureFeature(renderer, cameraData);
                cameraFailure = null;
                bool any = false;
                GeometryUtility.CalculateFrustumPlanes(camera, modelFrustum);
                foreach (var target in targets)
                {
                    target.Ready = false;
                    if (!VisibleIn(target, camera, modelFrustum)) continue;
                    try
                    {
                        // Check the cheap epoch for each camera/target, including
                        // changes after an earlier camera in this same frame.
                        {
                            long measured = RenderTiming.Start();
                            if (isolation == null) isolation = new LayerIsolation();
                            isolation.Refresh(environment.ModelMask, environment.Scene);
                            isolationFrame = Time.frameCount;
                            RenderTiming.End(RenderTiming.Part.Layers, measured);
                        }
                        target.Ready = RenderPrivate(context, camera, target); any |= target.Ready;
                    }
                    catch (Exception error)
                    {
                        cameraFailure = "private render " + target.Host.name + ": " + error.GetBaseException().Message;
                        Failure("private render " + target.Host.name, error);
                    }
                }
                if (!any) return;
                worldCamera = camera;
                composedFrame = -1;
            }
            catch (Exception error)
            {
                worldCamera = null;
                Failure("camera " + camera.name, error);
            }
        }

        static void EndCamera(ScriptableRenderContext context, Camera camera)
        {
            if (DirectRendering) { EndDirectCamera(camera); return; }
            if (renderingPrivate) return;
            if (camera == worldCamera)
            {
                if (composedFrame != Time.frameCount && cameraFailure == null)
                    Report("Lobby rendering: camera did not execute its model pass");
                worldCamera = null;
            }
            if (lobbySource && captureFrame == Time.frameCount && lobbyMembers.Count != 0 &&
                camera == lobbyMembers[lobbyMembers.Count - 1])
            {
                if (pendingEnvironment != null && pendingEnvironment.Ready && pendingViews.Count == lobbyMembers.Count)
                {
                    if (environment != null) environment.Dispose();
                    foreach (var view in views) view.Dispose();
                    environment = pendingEnvironment;
                    isolation = null;
                    pendingEnvironment = null;
                    views.Clear(); views.AddRange(pendingViews); pendingViews.Clear();
                    ++baselineGeneration;
                    Report("Lobby rendering: captured actual lobby (" + views.Count + " cameras)");
                    Debug.Log("[FTREZ-LOBBY] baseline=" + baselineGeneration + " source=" + lobbySource.name + " camera=" + lobbyCamera.name);
                    for (int i = 0; i < views.Count; ++i) views[i].LogDescription(i);
                }
                else Report("Lobby rendering: retained; incomplete lobby camera capture " + pendingViews.Count + "/" + lobbyMembers.Count);
                // Lighting snapshots are expensive renderer/Volume clones.
                // Refresh on source changes, plus a slow fallback for live lobby lighting.
                nextCapture = Time.realtimeSinceStartup + 10f;
                DiscardPending();
            }
        }

        internal static void Capture(Camera camera, ref RenderingData data)
        {
            if (DirectRendering) { CaptureDirect(camera, ref data); return; }
            if (!lobbySource || captureFrame != Time.frameCount || captureStage < 0 ||
                captureStage != pendingViews.Count || lobbyMembers[captureStage] != camera) return;
            try
            {
                if (captureStage == 0)
                {
                    pendingEnvironment = new LobbyEnvironment();
                    // Inspect the original camera's existing cull result once
                    // per snapshot; no extra cull or scene enumeration.
                    var probes = data.cullResults.visibleReflectionProbes;
                    for (int i = 0; i < probes.Length; ++i)
                        Debug.Log("[FTREZ-LOBBY] lobby reflection probe=" + probes[i].reflectionProbe +
                            " texture=" + LobbyEnvironment.DescribeTexture(probes[i].texture) + " hdr=" + probes[i].hdrData);
                    pendingEnvironment.Capture(camera, lobbySource, captureRenderers);
                }
                pendingViews.Add(LobbyView.Capture(camera, ref data));
            }
            catch (Exception error)
            {
                DiscardPending();
                nextCapture = Time.realtimeSinceStartup + 1f;
                Failure("lobby capture", error);
            }
        }

        static void DiscardPending()
        {
            if (pendingEnvironment != null) pendingEnvironment.Dispose();
            pendingEnvironment = null;
            foreach (var view in pendingViews) view.Dispose();
            pendingViews.Clear(); lobbyMembers.Clear(); captureFrame = captureStage = -1;
        }

        static RenderTexture NewTarget(string name, int width, int height, RenderTextureFormat format, int depth, int samples = 1)
        {
            if (!SystemInfo.SupportsRenderTextureFormat(format))
                throw new NotSupportedException("Render target " + format + " unsupported on " + SystemInfo.graphicsDeviceType);
            var result = new RenderTexture(width, height, depth, format, RenderTextureReadWrite.Linear)
            {
                name = name, hideFlags = HideFlags.HideAndDontSave,
                filterMode = FilterMode.Bilinear, wrapMode = TextureWrapMode.Clamp,
                useMipMap = false, autoGenerateMips = false, antiAliasing = samples
            };
            if (!result.Create()) { DestroyOwned(result); throw new InvalidOperationException("Lobby target allocation failed"); }
            return result;
        }

        static RenderTexture NewColorTarget(string name, int width, int height, GraphicsFormat format, int samples)
        {
            if (!SystemInfo.IsFormatSupported(format, FormatUsage.Render) || !SystemInfo.IsFormatSupported(format, FormatUsage.Sample))
                throw new NotSupportedException("Lobby color format " + format + " unsupported on " + SystemInfo.graphicsDeviceType);
            var descriptor = new RenderTextureDescriptor(width, height, format, 24) { msaaSamples = samples };
            var result = new RenderTexture(descriptor)
            {
                name = name, hideFlags = HideFlags.HideAndDontSave,
                filterMode = FilterMode.Bilinear, wrapMode = TextureWrapMode.Clamp
            };
            if (!result.Create()) { DestroyOwned(result); throw new InvalidOperationException("Lobby color target allocation failed"); }
            return result;
        }

        static void ReleaseTarget(ref RenderTexture target)
        {
            if (target) { target.Release(); DestroyOwned(target); }
            target = null;
        }

        static void ReleaseProduct(Target target)
        {
            if (target.Original != null) { target.Original.Dispose(); target.Original = null; }
            target.Sizing.Reset();
            ReleaseBuffers(target);
        }

        static void ReleaseBuffers(Target target)
        {
            foreach (var spare in target.Spare) ReleaseSpare(spare);
            target.Spare.Clear();
            if (privateCamera && (privateCamera.targetTexture == target.ColorA || privateCamera.targetTexture == target.ColorB))
                privateCamera.targetTexture = null;
            ReleaseTarget(ref target.ColorA); ReleaseTarget(ref target.ColorB); ReleaseTarget(ref target.Coverage);
            DestroyOwned(target.Composite); target.Composite = null; target.Ready = false;
        }

        static void ReleaseSpare(SpareBuffers spare)
        {
            ReleaseTarget(ref spare.ColorA); ReleaseTarget(ref spare.ColorB); ReleaseTarget(ref spare.Coverage);
        }

        static bool BuffersMatch(RenderTexture color, RenderTexture coverage, int width, int height,
                                 GraphicsFormat format, int samples)
        {
            return color && color.IsCreated() && color.width == width && color.height == height &&
                color.graphicsFormat == format && color.antiAliasing == samples &&
                coverage && coverage.IsCreated() && coverage.width == width && coverage.height == height;
        }

        static void ResizeBuffers(Target target, int width, int height, GraphicsFormat format, int samples, bool colorB)
        {
            // Cache only allocation storage; every acquired target is rendered
            // again this frame. Keep slots separate so pending composites cannot
            // observe another character overwriting their textures.
            SpareBuffers found = null;
            for (int i = target.Spare.Count - 1; i >= 0; --i)
            {
                var spare = target.Spare[i];
                if (BuffersMatch(spare.ColorA, spare.Coverage, width, height, format, samples) &&
                    (!colorB || (spare.ColorB && spare.ColorB.IsCreated())))
                { found = spare; target.Spare.RemoveAt(i); break; }
            }
            if (privateCamera && (privateCamera.targetTexture == target.ColorA || privateCamera.targetTexture == target.ColorB))
                privateCamera.targetTexture = null;
            if (target.ColorA)
            {
                // RGBA16F + depth, optional MSAA resolve, and RGFloat coverage.
                // Use a conservative bound; never retain oversized screen RTs.
                long bytes = (long)target.ColorA.width * target.ColorA.height *
                    ((target.ColorB ? 2 : 1) * ((long)GraphicsFormatUtility.GetBlockSize(target.ColorA.graphicsFormat) + 8) *
                     (target.Samples > 1 ? target.Samples + 1 : 1) + 8);
                target.Spare.Add(new SpareBuffers { ColorA = target.ColorA, ColorB = target.ColorB,
                    Coverage = target.Coverage, Bytes = bytes });
            }
            else { ReleaseTarget(ref target.ColorB); ReleaseTarget(ref target.Coverage); }
            target.ColorA = target.ColorB = target.Coverage = null;
            long retained = 0;
            for (int i = target.Spare.Count - 1; i >= 0; --i)
            {
                var spare = target.Spare[i];
                if (retained + spare.Bytes > 16L * 1024 * 1024)
                { ReleaseSpare(spare); target.Spare.RemoveAt(i); }
                else retained += spare.Bytes;
            }
            while (target.Spare.Count > 2) { ReleaseSpare(target.Spare[0]); target.Spare.RemoveAt(0); }
            target.Samples = samples; target.Ready = false;
            if (found != null)
            { target.ColorA = found.ColorA; target.ColorB = found.ColorB; target.Coverage = found.Coverage; }
            else
            {
                target.ColorA = NewColorTarget("FTREZ.Lobby.ColorA", width, height, format, samples);
                if (colorB) target.ColorB = NewColorTarget("FTREZ.Lobby.ColorB", width, height, format, samples);
                target.Coverage = NewTarget("FTREZ.Lobby.CoverageDepth", width, height, RenderTextureFormat.RGFloat, 0);
                target.Coverage.filterMode = FilterMode.Point;
            }
            if (!colorB) ReleaseTarget(ref target.ColorB);
            if (!target.Composite) target.Composite = new Material(coverageMaterial.shader) { hideFlags = HideFlags.HideAndDontSave };
        }

        internal static RenderTexture GetModelTarget(int slot, string name)
        {
            var target = targets[slot];
            if (name == "colorA") return target.ColorA;
            if (name == "colorB") return target.ColorB;
            if (name == "coverage") return target.Coverage;
            return foreground;
        }

        static void PrepareTargets(Camera camera, Target target)
        {
            float scale = camera.targetTexture ? 1 : UniversalRenderPipeline.asset.renderScale;
            int width = Mathf.Max(1, (int)(camera.pixelWidth * scale)), height = Mathf.Max(1, (int)(camera.pixelHeight * scale));
            if (width <= 0 || height <= 0 || width > SystemInfo.maxTextureSize || height > SystemInfo.maxTextureSize)
                throw new InvalidOperationException("Unsupported camera target size");
            int fullWidth = width, fullHeight = height;
            target.Viewport = ModelViewport(camera, target, fullWidth, fullHeight);
            width = Mathf.RoundToInt(fullWidth * target.Viewport.width);
            height = Mathf.RoundToInt(fullHeight * target.Viewport.height);
            target.FullWidth = fullWidth; target.FullHeight = fullHeight;
            int samples = views[0].Samples;
            var colorFormat = views[0].ColorFormat;
            // Retain the existing packed-HDR compatibility target. Coverage
            // now comes from depth, independently of the stored color alpha.
            if (colorFormat == GraphicsFormat.B10G11R11_UFloatPack32) colorFormat = GraphicsFormat.R16G16B16A16_SFloat;
            if (!GraphicsFormatUtility.HasAlphaChannel(colorFormat))
                throw new NotSupportedException("Lobby color format unsupported: " + colorFormat);
            var descriptor = new RenderTextureDescriptor(width, height, colorFormat, 24) { msaaSamples = samples };
            samples = SystemInfo.GetRenderTextureSupportedMSAASampleCount(descriptor);
            bool needsColorB = false;
            for (int i = 1; i < views.Count; ++i)
                needsColorB |= views[i].PostProcessing || views[i].HasFeatures;
            if (!foreground || foreground.width != fullWidth || foreground.height != fullHeight || !foreground.IsCreated())
            {
                ReleaseTarget(ref foreground);
                foreground = NewTarget("FTREZ.Lobby.TransparentDepth", fullWidth, fullHeight, RenderTextureFormat.RFloat, 24);
                foreground.filterMode = FilterMode.Point;
            }
            if (BuffersMatch(target.ColorA, target.Coverage, width, height, colorFormat, samples))
            {
                if (needsColorB && (!target.ColorB || !target.ColorB.IsCreated()))
                {
                    ReleaseTarget(ref target.ColorB);
                    target.ColorB = NewColorTarget("FTREZ.Lobby.ColorB", width, height, colorFormat, samples);
                }
                else if (!needsColorB) ReleaseTarget(ref target.ColorB);
                return;
            }
            ResizeBuffers(target, width, height, colorFormat, samples, needsColorB);
        }

        static Rect ModelViewport(Camera camera, Target target, int width, int height)
        {
            var full = new Rect(0, 0, 1, 1);
            if (Mathf.Max(width, height) <= 512) return full;
            Vector2 min = Vector2.one, max = Vector2.zero;
            bool found = false;
            foreach (var renderer in target.Renderers) if (Visible(renderer))
            {
                var bounds = renderer.bounds;
                for (int i = 0; i < 8; ++i)
                {
                    cropCorners[i] = bounds.center + Vector3.Scale(bounds.extents,
                        new Vector3((i & 1) == 0 ? -1 : 1, (i & 2) == 0 ? -1 : 1, (i & 4) == 0 ? -1 : 1));
                    var point = cropProjected[i] = camera.WorldToViewportPoint(cropCorners[i]);
                    if (point.z < camera.nearClipPlane) continue;
                    min = Vector2.Min(min, point); max = Vector2.Max(max, point); found = true;
                }
                // Clip the twelve bounds edges against the near plane. A foot
                // or oversized skin bound behind it need not force full-screen.
                for (int i = 0; i < 8; ++i) for (int axis = 1; axis <= 4; axis <<= 1)
                {
                    if ((i & axis) != 0) continue;
                    int j = i | axis;
                    float a = cropProjected[i].z, b = cropProjected[j].z, near = camera.nearClipPlane;
                    if ((a < near) == (b < near)) continue;
                    var point = camera.WorldToViewportPoint(Vector3.LerpUnclamped(cropCorners[i], cropCorners[j], (near - a) / (b - a)));
                    min = Vector2.Min(min, point); max = Vector2.Max(max, point); found = true;
                }
            }
            if (!found) return full;
            // Keep native pixel density and a 64-pixel effects margin. Quantize
            // allocations, then shrink with hysteresis after moving away.
            min = new Vector2(Mathf.Clamp01(min.x), Mathf.Clamp01(min.y));
            max = new Vector2(Mathf.Clamp01(max.x), Mathf.Clamp01(max.y));
            int w = Mathf.Min(width, Mathf.CeilToInt(((max.x - min.x) * width + 128) / 64) * 64);
            int h = Mathf.Min(height, Mathf.CeilToInt(((max.y - min.y) * height + 128) / 64) * 64);
            if (target.ColorA && target.FullWidth == width && target.FullHeight == height)
                target.Sizing.Fit(ref w, ref h, target.ColorA.width, target.ColorA.height, Time.realtimeSinceStartup);
            else target.Sizing.Reset();
            int x = Mathf.Clamp(Mathf.FloorToInt((min.x + max.x) * .5f * width - w * .5f), 0, width - w);
            int y = Mathf.Clamp(Mathf.FloorToInt((min.y + max.y) * .5f * height - h * .5f), 0, height - h);
            return new Rect((float)x / width, (float)y / height, (float)w / width, (float)h / height);
        }

        static bool RenderPrivate(ScriptableRenderContext context, Camera camera, Target target)
        {
            long measured = RenderTiming.Start();
            PrepareTargets(camera, target);
            RenderTiming.End(RenderTiming.Part.Targets, measured);
            measured = RenderTiming.Start();
            if (target.Original == null || !target.Original.Matches(target.Renderers))
            {
                if (target.Original != null) target.Original.Dispose();
                target.Original = new OriginalModel(target.Renderers);
            }
            RenderTiming.End(RenderTiming.Part.OriginalBinding, measured);
            if (!privateCamera)
            {
                var go = new GameObject("FTREZ.LobbyCamera") { hideFlags = HideFlags.HideAndDontSave };
                if (Application.isPlaying) Object.DontDestroyOnLoad(go);
                privateCamera = go.AddComponent<Camera>(); privateCamera.enabled = false;
                privateData = go.AddComponent<UniversalAdditionalCameraData>();
            }
            coverageFrame = -1;
            var oldActive = RenderTexture.active;
            renderingPrivate = true; activeTarget = target;
            long started = System.Diagnostics.Stopwatch.GetTimestamp();
            try
            {
                measured = RenderTiming.Start();
                target.Original.Borrow(isolation);
                RenderTiming.End(RenderTiming.Part.OriginalState, measured);
                measured = RenderTiming.Start();
                using (environment.Apply(target.Host, target.Original, target.Renderers, isolation))
                {
                    RenderTiming.End(RenderTiming.Part.Environment, measured);
                    RenderTexture source = null;
                    int rendered = 0;
                    for (int i = 0; i < views.Count; ++i)
                    {
                        var view = views[i];
                        if (i > 0 && !view.PostProcessing && !view.HasFeatures) continue;
                        privateStage = rendered;
                        privateCamera.CopyFrom(camera);
                        privateCamera.scene = environment.Scene;
                        // Include the original model in its retained scene plus
                        // the captured lights. Free draw layers isolate geometry;
                        // no hierarchy is moved between scenes for rendering.
                        privateCamera.overrideSceneCullingMask = ulong.MaxValue;
                        privateCamera.enabled = false;
                        privateCamera.transform.SetPositionAndRotation(camera.transform.position, camera.transform.rotation);
                        privateCamera.worldToCameraMatrix = camera.worldToCameraMatrix;
                        var viewport = target.Viewport;
                        privateCamera.projectionMatrix = CropMatrix(viewport) * camera.projectionMatrix;
                        privateCamera.rect = new Rect(0, 0, 1, 1);
                        privateCamera.targetTexture = rendered % 2 == 0 ? target.ColorA : target.ColorB;
                        privateCamera.clearFlags = CameraClearFlags.SolidColor;
                        privateCamera.backgroundColor = Color.clear;
                        privateCamera.cullingMask = rendered == 0 ? isolation.Mask : 0;
                        privateCamera.useOcclusionCulling = false;
                        privateCamera.stereoTargetEye = StereoTargetEyeMask.None;
                        privateData.renderType = CameraRenderType.Base;
                        privateData.requiresDepthTexture = true;
                        seed = source;
                        measured = RenderTiming.Start();
                        using (view.Apply(privateCamera, isolation))
                        {
                            EnsureFeature(privateData.scriptableRenderer, privateData);
                            RenderTiming.End(RenderTiming.Part.ViewSetup, measured);
                            measured = RenderTiming.Start();
#pragma warning disable 618
                            UniversalRenderPipeline.RenderSingleCamera(context, privateCamera);
#pragma warning restore 618
                            RenderTiming.End(rendered == 0 ? RenderTiming.Part.Stage0 : RenderTiming.Part.StageLater, measured);
                        }
                        source = privateCamera.targetTexture;
                        ++rendered;
                        ++privateStages;
                    }
                    if (coverageFrame != Time.frameCount || !source)
                        throw new InvalidOperationException("Private model pass did not execute");
                    target.Composite.SetTexture("_LobbyColor", source);
                    target.Composite.SetTexture("_LobbyCoverage", target.Coverage);
                    target.Composite.SetTexture("_LobbyForeground", foreground);
                    target.Composite.SetVector("_LobbyViewport", new Vector4(target.Viewport.x, target.Viewport.y, target.Viewport.width, target.Viewport.height));
                    int worldSamples = camera.targetTexture ? camera.targetTexture.antiAliasing : UniversalRenderPipeline.asset.msaaSampleCount;
                    target.Composite.SetFloat("_LobbyDepthSampling", target.Samples > 1 || (camera.allowMSAA && worldSamples > 1) ? 1 : 0);
                }
                return true;
            }
            finally
            {
                target.Original.Restore();
                seed = null;
                renderingPrivate = false; activeTarget = null;
                RenderTexture.active = oldActive;
                privateMilliseconds += (System.Diagnostics.Stopwatch.GetTimestamp() - started) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
                ++privateCalls;
                if (Time.realtimeSinceStartup >= nextTiming)
                {
                    Debug.Log("[FTREZ-LOBBY] " + Revision + " isolated render CPU submit avg=" +
                        (privateMilliseconds / privateCalls).ToString("F2") + "ms calls=" + privateCalls +
                        " stages=" + privateStages + " size=" + target.ColorA.width + "x" + target.ColorA.height);
                    Debug.Log("[FTREZ-LOBBY] CPU parts avg/calls:" + RenderTiming.Flush() +
                        " layerQueriesTotal=" + (isolation == null ? 0 : isolation.RefreshCount) +
                        " geometryCache=" + GeometryChanges.Read(out _, out _));
                    privateMilliseconds = 0; privateCalls = privateStages = 0; nextTiming = Time.realtimeSinceStartup + 5;
                }
            }
        }

        internal static void Enqueue(ScriptableRenderer renderer, ref RenderingData data,
                                     LobbyFeature.CapturePass capturePass, LobbyFeature.SeedPass seedPass,
                                     LobbyFeature.CoveragePass coveragePass, LobbyFeature.ForegroundPass foregroundPass,
                                     LobbyFeature.CompositePass compositePass)
        {
            var camera = data.cameraData.camera;
            if (DirectRendering)
            {
                if (camera == lobbyCamera && captureFrame == Time.frameCount)
                    renderer.EnqueuePass(capturePass);
                return;
            }
            if (renderingPrivate && camera == privateCamera)
            {
                if (!loggedProbes)
                {
                    var probes = data.cullResults.visibleReflectionProbes;
                    Debug.Log("[FTREZ-LOBBY] private cull reflection probes=" + probes.Length);
                    for (int i = 0; i < probes.Length; ++i)
                        Debug.Log("[FTREZ-LOBBY] private reflection=" + LobbyEnvironment.DescribeTexture(probes[i].texture) + " hdr=" + probes[i].hdrData);
                    loggedProbes = true;
                }
                if (seed) { seedPass.Source = seed; renderer.EnqueuePass(seedPass); }
                if (privateStage == 0) renderer.EnqueuePass(coveragePass);
            }
            else if (lobbySource && captureFrame == Time.frameCount && lobbyMembers.Contains(camera))
                renderer.EnqueuePass(capturePass);
            else if (camera == worldCamera)
            {
                foregroundPass.Target = foreground;
                foregroundPass.renderPassEvent = beforeUI ? (RenderPassEvent)((int)RenderPassEvent.BeforeRenderingTransparents - 1) : RenderPassEvent.AfterRenderingTransparents;
                compositePass.renderPassEvent = beforeUI ? RenderPassEvent.BeforeRenderingTransparents : RenderPassEvent.AfterRenderingPostProcessing;
                renderer.EnqueuePass(foregroundPass);
                renderer.EnqueuePass(compositePass);
            }
        }

        internal static void CopyCoverage(ScriptableRenderContext context, ref RenderingData data)
        {
            var renderer = data.cameraData.renderer;
            var command = CommandBufferPool.Get("FTREZ lobby coverage/depth");
            try
            {
                command.Blit(renderer.cameraColorTargetHandle.nameID, activeTarget.Coverage, coverageMaterial, 0);
                context.ExecuteCommandBuffer(command);
                coverageFrame = Time.frameCount;
            }
            finally { CommandBufferPool.Release(command); }
        }

        internal static void Composite(ScriptableRenderContext context, ref RenderingData data)
        {
            if (data.cameraData.camera != worldCamera) return;
            var command = CommandBufferPool.Get("FTREZ lobby self before UI");
            try
            {
                var color = data.cameraData.renderer.cameraColorTargetHandle.nameID;
                int count = 0;
                foreach (var target in targets)
                {
                    if (!target.Ready || !target.Composite) continue;
                    // The shader only writes visible, fully covered model RGB.
                    // Rejected pixels and destination alpha remain untouched.
                    var descriptor = data.cameraData.cameraTargetDescriptor;
                    DrawComposite(command, target.ColorA, color, target.Composite, target.Viewport,
                        descriptor.width, descriptor.height);
                    ++count;
                }
                if (count == 0) return;
                context.ExecuteCommandBuffer(command);
                composedFrame = Time.frameCount;
                lastCompositeTime = Time.realtimeSinceStartup;
                if (activeCount != count || activeGeneration != baselineGeneration || activeFailure != cameraFailure)
                {
                    activeCount = count; activeGeneration = baselineGeneration; activeFailure = cameraFailure;
                    activeMessage = "Lobby rendering: active on " + count + " model(s); baseline " + baselineGeneration +
                        (cameraFailure == null ? "" : "; fallback: " + cameraFailure);
                }
                Report(activeMessage);
            }
            catch (Exception error)
            {
                cameraFailure = "composite: " + error.GetBaseException().Message;
                Failure("composite", error);
            }
            finally { CommandBufferPool.Release(command); }
        }

        internal static Material CoverageMaterial { get { return coverageMaterial; } }

        internal static Matrix4x4 CropMatrix(Rect viewport)
        {
            var crop = Matrix4x4.identity;
            crop.m00 = 1 / viewport.width; crop.m11 = 1 / viewport.height;
            crop.m03 = (1 - 2 * viewport.x - viewport.width) / viewport.width;
            crop.m13 = (1 - 2 * viewport.y - viewport.height) / viewport.height;
            return crop;
        }

        internal static Rect PixelRegion(Rect viewport, int width, int height)
        {
            return Rect.MinMaxRect(Mathf.Clamp(Mathf.Floor(viewport.xMin * width), 0, width),
                Mathf.Clamp(Mathf.Floor(viewport.yMin * height), 0, height),
                Mathf.Clamp(Mathf.Ceil(viewport.xMax * width), 0, width),
                Mathf.Clamp(Mathf.Ceil(viewport.yMax * height), 0, height));
        }

        internal static void DrawComposite(CommandBuffer command, RenderTexture source,
            RenderTargetIdentifier destination, Material material, Rect viewport, int width, int height)
        {
            // Scissor retains the existing full-screen UV/depth conventions but
            // rejects fragments outside the crop before running the shader.
            var pixels = PixelRegion(viewport, width, height);
            command.EnableScissorRect(pixels);
            command.Blit(source, destination, material, 1);
            command.DisableScissorRect();
            RenderTiming.Pixels(false, pixels, width, height);
        }

        internal static Rect ForegroundRegion()
        {
            bool any = false;
            var region = new Rect();
            foreach (var target in targets) if (target.Ready)
            {
                var rect = target.Viewport;
                region = any ? Rect.MinMaxRect(Mathf.Min(region.xMin, rect.xMin), Mathf.Min(region.yMin, rect.yMin),
                    Mathf.Max(region.xMax, rect.xMax), Mathf.Max(region.yMax, rect.yMax)) : rect;
                any = true;
            }
            return region;
        }

        internal static bool HasReplacementForeground()
        {
            foreach (var target in targets) if (target.Ready)
                foreach (var renderer in target.Renderers) if (Visible(renderer))
                {
                    renderer.GetSharedMaterials(foregroundMaterials);
                    foreach (var material in foregroundMaterials)
                        if (material && material.renderQueue > 2500) return true;
                }
            return false;
        }

        internal static void ForegroundFailure(Exception error)
        {
            foreach (var target in targets) target.Ready = false;
            cameraFailure = "foreground: " + error.GetBaseException().Message;
            Failure("foreground", error);
        }

        internal static IDisposable ExcludeReplacementForeground()
        {
            return new ForegroundScope();
        }

        sealed class ForegroundScope : IDisposable
        {
            readonly List<Renderer> hidden = new List<Renderer>();
            public ForegroundScope()
            {
                try
                {
                    foreach (var target in targets) if (target.Ready)
                        foreach (var renderer in target.Renderers)
                            if (Visible(renderer)) { hidden.Add(renderer); renderer.forceRenderingOff = true; }
                }
                catch { Dispose(); throw; }
            }
            public void Dispose()
            {
                foreach (var renderer in hidden) if (renderer) renderer.forceRenderingOff = false;
            }
        }
    }

    public sealed class LobbyFeature : ScriptableRendererFeature
    {
        CapturePass capture; SeedPass seed; CoveragePass coverage; ForegroundPass foreground; CompositePass composite;
        public override void Create()
        {
            capture = new CapturePass(); seed = new SeedPass();
            coverage = new CoveragePass(); foreground = new ForegroundPass(); composite = new CompositePass();
        }
        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData data)
        {
            Entry.Enqueue(renderer, ref data, capture, seed, coverage, foreground, composite);
        }
        public sealed class CapturePass : ScriptableRenderPass
        {
            public CapturePass() { renderPassEvent = (RenderPassEvent)((int)RenderPassEvent.BeforeRenderingOpaques - 1); }
            public override void Execute(ScriptableRenderContext context, ref RenderingData data)
            { Entry.Capture(data.cameraData.camera, ref data); }
        }
        public sealed class SeedPass : ScriptableRenderPass
        {
            public RenderTexture Source;
            public SeedPass() { renderPassEvent = RenderPassEvent.BeforeRenderingOpaques; }
            public override void Execute(ScriptableRenderContext context, ref RenderingData data)
            {
                var cmd = CommandBufferPool.Get("FTREZ lobby camera stack input");
                try
                {
                    cmd.Blit(Source, data.cameraData.renderer.cameraColorTargetHandle.nameID);
                    context.ExecuteCommandBuffer(cmd);
                }
                finally { CommandBufferPool.Release(cmd); }
            }
        }
        public sealed class CoveragePass : ScriptableRenderPass
        {
            public CoveragePass()
            {
                renderPassEvent = (RenderPassEvent)((int)RenderPassEvent.BeforeRenderingPostProcessing - 1);
                ConfigureInput(ScriptableRenderPassInput.Depth);
            }
            public override void Execute(ScriptableRenderContext context, ref RenderingData data)
            { Entry.CopyCoverage(context, ref data); }
        }
        public sealed class CompositePass : ScriptableRenderPass
        {
            public CompositePass()
            {
                // Exactly 600 tells UniversalRenderer.Setup to keep an
                // intermediate color target until this pass has finished.
                renderPassEvent = RenderPassEvent.AfterRenderingPostProcessing;
                // We write into camera color but never sample its opaque copy.
                // Color input would force URP's extra full-screen CopyColor pass.
                ConfigureInput(ScriptableRenderPassInput.Depth);
            }
            public override void Execute(ScriptableRenderContext context, ref RenderingData data)
            {
                long measured = RenderTiming.Start();
                try { Entry.Composite(context, ref data); }
                finally { RenderTiming.End(RenderTiming.Part.Composite, measured); }
            }
        }
        public sealed class ForegroundPass : ScriptableRenderPass
        {
            public RenderTexture Target;
            readonly Plane[] cullPlanes = new Plane[6];
            static readonly ShaderTagId[] tags = { new ShaderTagId("UniversalForward"), new ShaderTagId("UniversalForwardOnly"),
                new ShaderTagId("SRPDefaultUnlit"), new ShaderTagId("LightweightForward") };
            public ForegroundPass() { renderPassEvent = RenderPassEvent.AfterRenderingTransparents; }
            public override void OnCameraSetup(CommandBuffer command, ref RenderingData data)
            {
                ConfigureTarget(new RenderTargetIdentifier(Target), new RenderTargetIdentifier(Target));
                ConfigureClear(ClearFlag.All, new Color(1e20f, 0, 0, 0));
            }
            public override void Execute(ScriptableRenderContext context, ref RenderingData data)
            {
                long measured = RenderTiming.Start();
                var region = Entry.ForegroundRegion();
                if (region.width <= 0 || region.height <= 0) return;
                var pixels = Entry.PixelRegion(region, Target.width, Target.height);
                var command = CommandBufferPool.Get("FTREZ foreground crop");
                // Conservative depth barrier: retain the normal game result
                // wherever transparent foreground geometry overlaps the model.
                // This deliberately includes alpha holes in unknown shaders.
                var sorting = new SortingSettings(data.cameraData.camera) { criteria = SortingCriteria.CommonOpaque };
                var drawing = new DrawingSettings(tags[0], sorting) { overrideMaterial = Entry.CoverageMaterial, overrideMaterialPassIndex = 2 };
                for (int i = 1; i < tags.Length; ++i) drawing.SetShaderPassName(i, tags[i]);
                var filtering = new FilteringSettings(RenderQueueRange.transparent, data.cameraData.camera.cullingMask);
                try
                {
                    command.EnableScissorRect(pixels);
                    context.ExecuteCommandBuffer(command); command.Clear();
                    RenderTiming.Pixels(true, pixels, Target.width, Target.height);
                    if (Entry.HasReplacementForeground())
                    {
                        // Visibility is baked into CullingResults. Changing it
                        // only around DrawRenderers leaves clothing in the mask.
                        using (Entry.ExcludeReplacementForeground())
                        {
                            if (!data.cameraData.camera.TryGetCullingParameters(out var parameters))
                                throw new InvalidOperationException("Cannot cull transparent foreground");
                            parameters.cullingOptions &= ~(CullingOptions.NeedsLighting | CullingOptions.ShadowCasters);
                            // Only barriers within the union of model crops can
                            // affect composition. Keep normal drawing matrices.
                            parameters.cullingMatrix = Entry.CropMatrix(region) * parameters.cullingMatrix;
                            GeometryUtility.CalculateFrustumPlanes(parameters.cullingMatrix, cullPlanes);
                            parameters.cullingPlaneCount = cullPlanes.Length;
                            for (int i = 0; i < cullPlanes.Length; ++i) parameters.SetCullingPlane(i, cullPlanes[i]);
                            var foreground = context.Cull(ref parameters);
                            context.DrawRenderers(foreground, ref drawing, ref filtering);
                        }
                    }
                    else context.DrawRenderers(data.cullResults, ref drawing, ref filtering);
                }
                catch (Exception error) { Entry.ForegroundFailure(error); }
                finally
                {
                    command.Clear(); command.DisableScissorRect();
                    try { context.ExecuteCommandBuffer(command); }
                    finally { CommandBufferPool.Release(command); RenderTiming.End(RenderTiming.Part.Foreground, measured); }
                }
            }
        }
    }
}
