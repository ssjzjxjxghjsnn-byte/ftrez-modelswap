using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using FTRez.LobbyModel;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.SceneManagement;
using UnityEngine.UI;
using Object = UnityEngine.Object;

// Synthetic render-target readback only: no screenshots or device operations.
public static partial class ValidateLobbyModel
{
    const int size = 192;
    static readonly List<string> report = new List<string>();
    static readonly List<string> errors = new List<string>();
    const BindingFlags flags = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;

    static void Require(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }

    static IEnumerator Frames(int count = 2)
    {
        int frame = Time.frameCount;
        double deadline = EditorApplication.timeSinceStartup + 15;
        while (Time.frameCount - frame < count)
        {
            Require(EditorApplication.timeSinceStartup < deadline, "Synthetic player loop did not advance");
            yield return null;
        }
    }

    sealed class Fixture : IDisposable
    {
        public Camera Camera, UI;
        public Canvas UICanvas;
        public Image UIImage;
        public Renderer Self, Other;
        public Light Light;
        public UniversalRenderPipelineAsset Pipeline;
        public UniversalRendererData RendererData;
        public Volume Volume;
        public ColorAdjustments Grade;
        public Tonemapping Tone;
        public readonly List<Object> Owned = new List<Object>();
        readonly Scene scene;
        public Fixture(bool hdr, bool basePost, bool overlayPost, bool ui, bool direct = false)
        {
            Entry.Shutdown();
            Entry.ValidateLegacyComposition = !direct;
            scene = SceneManager.CreateScene("Lobby test " + Guid.NewGuid());
            SceneManager.SetActiveScene(scene);
            RenderSettings.ambientMode = AmbientMode.Flat; RenderSettings.ambientLight = new Color(.15f, .1f, .08f);
            RenderSettings.ambientIntensity = 1; RenderSettings.reflectionIntensity = 0; RenderSettings.fog = false;
            var renderer = ScriptableObject.CreateInstance<UniversalRendererData>(); Owned.Add(renderer);
            RendererData = renderer;
            typeof(UniversalRendererData).GetMethod("ReloadAllNullProperties", flags).Invoke(renderer, null);
            renderer.postProcessData = AssetDatabase.LoadAssetAtPath<PostProcessData>("Packages/com.unity.render-pipelines.universal/Runtime/Data/PostProcessData.asset");
            Require(renderer.postProcessData, "URP post process resources missing");
            renderer.intermediateTextureMode = IntermediateTextureMode.Always;
            Pipeline = UniversalRenderPipelineAsset.Create(renderer); Owned.Add(Pipeline);
            Pipeline.msaaSampleCount = 1; Pipeline.supportsHDR = hdr;
            GraphicsSettings.defaultRenderPipeline = Pipeline; QualitySettings.renderPipeline = Pipeline;
            var root = new GameObject("Lobby fixture");
            Camera = new GameObject("Role camera").AddComponent<Camera>(); Camera.transform.SetParent(root.transform);
            Camera.tag = "MainCamera"; Camera.transform.position = new Vector3(0, 0, -5);
            Camera.fieldOfView = 35; Camera.nearClipPlane = .1f; Camera.farClipPlane = 40;
            Camera.clearFlags = CameraClearFlags.SolidColor; Camera.backgroundColor = new Color(.03f, .04f, .07f, 1);
            Camera.cullingMask &= ~(1 << 5);
            Camera.allowHDR = hdr; Camera.allowMSAA = false;
            Camera.targetTexture = new RenderTexture(size, size, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            Camera.targetTexture.Create(); Owned.Add(Camera.targetTexture);
            var data = Camera.GetUniversalAdditionalCameraData(); data.renderPostProcessing = basePost;
            data.requiresDepthTexture = true; data.volumeLayerMask = 1;
            var material = new Material(Shader.Find("Universal Render Pipeline/Lit")); Owned.Add(material);
            material.SetColor("_BaseColor", new Color(.45f, .28f, .12f, 1)); material.SetFloat("_Smoothness", .15f);
            Self = GameObject.CreatePrimitive(PrimitiveType.Sphere).GetComponent<Renderer>();
            Self.name = "Self"; Self.transform.SetParent(root.transform); Self.transform.position = new Vector3(-.7f, 0, 0);
            Self.gameObject.layer = 8; Self.sharedMaterial = material;
            Other = GameObject.CreatePrimitive(PrimitiveType.Sphere).GetComponent<Renderer>();
            Other.name = "Other"; Other.transform.position = new Vector3(.7f, 0, 0); Other.gameObject.layer = 9; Other.sharedMaterial = material;
            var block = new MaterialPropertyBlock(); block.SetFloat("_FTRezUserMarker", .42f); Self.SetPropertyBlock(block);
            Light = new GameObject("Lobby light").AddComponent<Light>(); Light.type = LightType.Directional;
            Light.transform.rotation = Quaternion.LookRotation(new Vector3(.25f, -.2f, 1));
            Light.intensity = 1.4f; Light.color = new Color(1, .85f, .65f); Light.shadows = LightShadows.None;
            RenderSettings.sun = Light;
            Volume = new GameObject("Volume").AddComponent<Volume>(); Volume.isGlobal = true;
            Volume.sharedProfile = ScriptableObject.CreateInstance<VolumeProfile>(); Owned.Add(Volume.sharedProfile);
            Grade = Volume.sharedProfile.Add<ColorAdjustments>(true); Grade.postExposure.value = .6f;
            Tone = Volume.sharedProfile.Add<Tonemapping>(true); Tone.mode.value = TonemappingMode.ACES;
            if (ui || overlayPost)
            {
                UI = new GameObject("UI camera").AddComponent<Camera>(); UI.CopyFrom(Camera);
                UI.transform.SetPositionAndRotation(Camera.transform.position, Camera.transform.rotation);
                // Match the base target's pixel dimensions before Unity lays
                // out ScreenSpaceCamera UI. URP still shares the stack target.
                UI.targetTexture = Camera.targetTexture; UI.cullingMask = 1 << 5; UI.tag = "Untagged";
                var extra = UI.GetUniversalAdditionalCameraData(); extra.renderType = CameraRenderType.Overlay;
                extra.renderPostProcessing = overlayPost; extra.volumeLayerMask = 1;
                data.cameraStack.Add(UI);
                if (ui)
                {
                    var canvas = new GameObject("Game UI", typeof(RectTransform), typeof(Canvas)).GetComponent<Canvas>();
                    UICanvas = canvas;
                    canvas.gameObject.layer = 5; canvas.renderMode = RenderMode.ScreenSpaceCamera;
                    canvas.worldCamera = UI; canvas.planeDistance = 1;
                    var image = new GameObject("UI image", typeof(RectTransform), typeof(Image)).GetComponent<Image>();
                    UIImage = image;
                    image.gameObject.layer = 5; image.transform.SetParent(canvas.transform, false);
                    image.color = new Color(.04f, .85f, .13f, 1);
                    var rect = image.rectTransform; rect.anchorMin = new Vector2(.25f, .45f); rect.anchorMax = new Vector2(.35f, .55f);
                    rect.offsetMin = rect.offsetMax = Vector2.zero;
                    Canvas.ForceUpdateCanvases();
                }
            }
            Entry.InitializeShader(AssetDatabase.LoadAssetAtPath<Shader>("Assets/LobbyModel/LobbyModel.shader"));
            // Batch mode has no GameView to emit native Canvas geometry for
            // explicit camera render requests. Use the same native emission
            // API URP uses for preview/reflection cameras; keep real UGUI.
            RenderPipelineManager.beginCameraRendering += EmitCanvas;
        }
        void EmitCanvas(ScriptableRenderContext context, Camera camera)
        {
            if (camera == UI && UICanvas)
                ScriptableRenderContext.EmitGeometryForCamera(camera);
        }
        public Color[] Render(Camera renderCamera = null)
        {
            if (!renderCamera) renderCamera = Camera;
            RenderPipeline.SubmitRenderRequest(renderCamera, new RenderPipeline.StandardRequest { destination = renderCamera.targetTexture });
            var previous = RenderTexture.active; RenderTexture.active = renderCamera.targetTexture;
            int width = renderCamera.targetTexture.width, height = renderCamera.targetTexture.height;
            var pixels = new Texture2D(width, height, TextureFormat.RGBAFloat, false, true);
            try
            {
                pixels.ReadPixels(new Rect(0, 0, width, height), 0, 0); pixels.Apply();
                var result = pixels.GetPixels();
                foreach (var color in result)
                    Require(!float.IsNaN(color.r) && !float.IsInfinity(color.r) && !float.IsNaN(color.g) && !float.IsInfinity(color.b), "Nonfinite render target");
                return result;
            }
            finally { Object.DestroyImmediate(pixels); RenderTexture.active = previous; }
        }
        public Color[] CaptureLobby()
        {
            Canvas.ForceUpdateCanvases(); Render(); Canvas.ForceUpdateCanvases();
            Entry.SetTarget(Self.transform, new[] { Self }, false);
            var result = Render();
            Require(Entry.GetStatus().Contains("captured actual lobby"), "Lobby capture did not finish: " + Entry.GetStatus());
            return result;
        }
        public void EnterMatch(bool bindSelf = true)
        {
            Pipeline.supportsHDR = false; Camera.allowHDR = false;
            Camera.GetUniversalAdditionalCameraData().renderPostProcessing = true;
            if (UI) UI.GetUniversalAdditionalCameraData().renderPostProcessing = false;
            Grade.postExposure.value = -1.5f; Tone.mode.value = TonemappingMode.Neutral;
            Light.color = new Color(.2f, .45f, 1); Light.intensity = .35f;
            RenderSettings.ambientLight = new Color(.01f, .025f, .06f);
            if (bindSelf) Entry.SetTarget(Self.transform, new[] { Self }, true);
        }
        public void AssertRestored()
        {
            Require(Self.gameObject.layer == 8 && Light.enabled, "Layer/light was not restored");
            Require(!Pipeline.supportsHDR && !Camera.allowHDR && Camera.GetUniversalAdditionalCameraData().renderPostProcessing, "World camera/pipeline was changed");
            Require(Self.lightProbeUsage == LightProbeUsage.BlendProbes && Self.shadowCastingMode == ShadowCastingMode.On && !Self.forceRenderingOff, "Renderer state was changed");
            var block = new MaterialPropertyBlock(); Self.GetPropertyBlock(block);
            Require(Mathf.Abs(block.GetFloat("_FTRezUserMarker") - .42f) < 1e-6f, "Property block was not restored");
            var field = typeof(UniversalRenderPipelineAsset).GetField("m_RendererDataList", flags);
            Require(((Array)field.GetValue(Pipeline)).Length == 1, "Private renderer slot leaked into pipeline");
        }
        public void Dispose()
        {
            RenderPipelineManager.beginCameraRendering -= EmitCanvas;
            Entry.Shutdown();
            if (Camera) Camera.targetTexture = null;
            if (UI) UI.targetTexture = null;
            GraphicsSettings.defaultRenderPipeline = null; QualitySettings.renderPipeline = null;
            foreach (var root in scene.GetRootGameObjects()) if (root) Object.DestroyImmediate(root);
            foreach (var item in Owned) if (item) Object.DestroyImmediate(item);
            SceneManager.UnloadSceneAsync(scene);
        }
    }

    static float Difference(Color[] a, Color[] b, int left, int bottom, int width, int height)
    {
        float result = 0;
        for (int y = bottom; y < bottom + height; ++y) for (int x = left; x < left + width; ++x)
        {
            var d = a[y * size + x] - b[y * size + x];
            result = Mathf.Max(result, Mathf.Abs(d.r), Mathf.Abs(d.g), Mathf.Abs(d.b));
        }
        return result;
    }

    static Color SampleTarget(string field, int x, int y)
    {
        var rt = Entry.GetModelTarget(0, field);
        var previous = RenderTexture.active; RenderTexture.active = rt;
        var texture = new Texture2D(1, 1, TextureFormat.RGBAFloat, false, true);
        try { texture.ReadPixels(new Rect(x, y, 1, 1), 0, 0); texture.Apply(); return texture.GetPixel(0, 0); }
        finally { Object.DestroyImmediate(texture); RenderTexture.active = previous; }
    }

    static IEnumerator Appearance(bool hdr, bool basePost, bool overlayPost, bool ui)
    {
        using (var f = new Fixture(hdr, basePost, overlayPost, ui))
        {
            yield return Frames();
            var lobby = f.CaptureLobby(); f.EnterMatch();
            var result = f.Render();
            Require((bool)Entry.GetModelTarget(0, "colorB") == overlayPost,
                "Second color buffer did not follow the captured camera stages");
            report.Add("Samples lobbySelf=" + lobby[82 * size + 52].ToString("G7") + " resultSelf=" + result[82 * size + 52].ToString("G7") +
                " privateSelf=" + SampleTarget("colorA", 52, 82).ToString("G7") + " maskSelf=" + SampleTarget("coverage", 52, 82).ToString("G7") +
                " privateOther=" + SampleTarget("colorA", 136, 82).ToString("G7") + " maskOther=" + SampleTarget("coverage", 136, 82).ToString("G7"));
            Require(Entry.GetStatus().Contains("active on"), "Composition did not execute: " + Entry.GetStatus());
            f.AssertRestored();
            Entry.SetTarget(null, null, true); var original = f.Render();
            report.Add("OriginalSelf=" + original[82 * size + 52].ToString("G7") + " OtherOriginal=" + original[82 * size + 136].ToString("G7") + " OtherResult=" + result[82 * size + 136].ToString("G7"));
            // Interior model pixels below the Canvas rectangle. UI is compared
            // with the current scene separately, since lobby UI may be graded.
            float self = Difference(result, lobby, 46, 78, 15, 6);
            float other = Difference(result, original, 129, 78, 15, 25);
            float changed = Difference(original, lobby, 46, 78, 15, 6);
            float uiError = 0; int uiPixels = 0;
            if (ui)
            {
                int greenTotal = 0, minX = size, minY = size, maxX = -1, maxY = -1;
                for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
                {
                    var color = original[y * size + x];
                    if (color.g < .15f || color.g < color.r * 4 || color.g < color.b * 2) continue;
                    ++greenTotal; minX = Mathf.Min(minX, x); minY = Mathf.Min(minY, y);
                    maxX = Mathf.Max(maxX, x); maxY = Mathf.Max(maxY, y);
                }
                var mesh = f.UIImage.canvasRenderer.GetMesh();
                report.Add($"UI fixture: total={greenTotal} bounds=({minX},{minY})-({maxX},{maxY}) rect={f.UIImage.rectTransform.rect} canvas={((RectTransform)f.UICanvas.transform).rect} camera={f.UI.pixelRect} vertices={(mesh ? mesh.vertexCount : 0)}");
                var corners = new Vector3[4]; f.UIImage.rectTransform.GetWorldCorners(corners);
                report.Add("UI projected corners=" + f.UI.WorldToViewportPoint(corners[0]) + " / " + f.UI.WorldToViewportPoint(corners[2]) +
                    " material=" + f.UIImage.materialForRendering.shader.name + " supported=" + f.UIImage.materialForRendering.shader.isSupported + " frame=" + Time.frameCount);
                for (int y = 55; y < 137; ++y) for (int x = 26; x < 79; ++x)
                {
                    var color = original[y * size + x];
                    if (color.g < .15f || color.g < color.r * 4 || color.g < color.b * 2) continue;
                    ++uiPixels; uiError = Mathf.Max(uiError, Difference(result, original, x, y, 1, 1));
                }
                report.Add("UI overlap pixels=" + uiPixels);
                Require(uiPixels > 50, "Fixture UI did not draw over Self");
            }
            report.Add($"Appearance HDR={hdr} BasePost={basePost} OverlayPost={overlayPost} UI={ui}: self={self:G7} other={other:G7} originalDifference={changed:G7} ui={uiError:G7}");
            Require(changed > .02f, "Fixture does not distinguish lobby from match");
            Require(self < .018f, "Self does not match lobby");
            Require(other < .002f && uiError < .002f, "Other character or UI changed");
        }
    }

    static IEnumerator Foreground(bool transparent, bool msaa = false)
    {
        using (var f = new Fixture(true, true, false, false))
        {
            var target = f.Camera.targetTexture;
            if (msaa)
            {
                f.Pipeline.msaaSampleCount = 4; f.Camera.allowMSAA = true;
                target.Release(); target.antiAliasing = 4; target.Create();
            }
            yield return Frames();
            f.CaptureLobby(); f.EnterMatch();
            if (msaa)
            {
                f.Pipeline.msaaSampleCount = 1; f.Camera.allowMSAA = false;
                target.Release(); target.antiAliasing = 1; target.Create();
            }
            var quad = GameObject.CreatePrimitive(PrimitiveType.Quad); quad.layer = 10;
            quad.transform.position = new Vector3(-.7f, 0, msaa ? -.53f : -.7f); quad.transform.localScale = new Vector3(.6f, .6f, 1);
            var material = new Material(Shader.Find("Universal Render Pipeline/Unlit")); f.Owned.Add(material);
            material.SetColor("_BaseColor", new Color(.1f, .7f, .25f, transparent ? .5f : 1));
            if (transparent)
            {
                material.SetFloat("_Surface", 1); material.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha);
                material.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha); material.SetFloat("_ZWrite", 0);
                material.EnableKeyword("_SURFACE_TYPE_TRANSPARENT"); material.renderQueue = 3000;
            }
            quad.GetComponent<Renderer>().sharedMaterial = material;
            var result = f.Render(); f.AssertRestored();
            if (msaa) Require(Entry.GetModelTarget(0, "colorA").antiAliasing > 1, "Foreground fixture lost lobby MSAA");
            Entry.SetTarget(null, null, true); var original = f.Render();
            float error = Difference(result, original, 40, 82, 20, 25);
            report.Add($"Foreground transparent={transparent} MSAA={msaa}: error={error:G7}");
            Require(error < .002f, "Foreground was overwritten");
        }
    }

    static IEnumerator RabbitInitialCapture()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            yield return Frames();
            f.Render();
            var hosts = new Transform[] { null, f.Other.transform, null };
            Entry.SetTargets(f.Self.transform, hosts, new[] { f.Other }, new[] { 1 }, true);
            var lobby = f.Render();
            Require(Entry.GetStatus().Contains("captured actual lobby"), "Rabbit alone did not capture its original lobby source");
            Require(!Entry.GetModelTarget(0, "colorA"), "Rabbit initial capture allocated a Self product");
            f.EnterMatch(false);
            Entry.SetTargets(null, hosts, new[] { f.Other }, new[] { 1 }, false);
            var original = f.Render();
            Entry.SetTargets(null, hosts, new[] { f.Other }, new[] { 1 }, true);
            var result = f.Render();
            Require(Difference(original, lobby, 129, 78, 15, 25) > .02f, "Rabbit initial fixture has no lighting difference");
            Require(Difference(result, lobby, 129, 78, 15, 25) < .018f, "Rabbit initial capture does not reproduce lobby");
            Require(Difference(result, original, 46, 78, 15, 6) < .002f, "Rabbit-only rendering changed Self");
            f.AssertRestored();
            report.Add("Rabbit initial capture with Self replacement never enabled: PASS");
        }
    }

    static IEnumerator FailureAndVisibility()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            yield return Frames();
            f.CaptureLobby(); f.EnterMatch();
            var views = (IList)typeof(Entry).GetField("views", flags).GetValue(null);
            var field = views[0].GetType().GetField("rendererData", flags);
            var saved = field.GetValue(views[0]); field.SetValue(views[0], null);
            Color[] failed;
            try { failed = f.Render(); Require(Entry.GetStatus().Contains("retained"), "Failure fallback did not run"); }
            finally { field.SetValue(views[0], saved); }
            f.AssertRestored();
            Entry.SetTarget(null, null, true); var original = f.Render();
            Require(Difference(failed, original, 0, 0, size, size) < .002f, "Render failure changed game output");
            f.Self.forceRenderingOff = true;
            Entry.SetTarget(f.Self.transform, new[] { f.Self }, true); var hidden = f.Render();
            Entry.SetTarget(null, null, true); var hiddenOriginal = f.Render();
            Require(Difference(hidden, hiddenOriginal, 0, 0, size, size) < .002f, "Hidden first-person geometry reappeared");
            Require(Resources.FindObjectsOfTypeAll<Camera>().Length < 8, "Private camera leaked after clear");
            report.Add("Failure cleanup and hidden geometry: PASS");
        }
    }

    static IEnumerator ExplicitSourceAndStatus()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            ShaderProbe.Verify(Entry.CoverageMaterial.shader);
            report.Add("Three-pass startup GPU probe and visible/occluded composite: PASS");
            yield return Frames();
            // The first camera under a shared root is unrelated to the lobby.
            var decoy = new GameObject("Unrelated camera").AddComponent<Camera>();
            decoy.transform.SetParent(f.Camera.transform.parent); decoy.transform.SetAsFirstSibling(); decoy.enabled = false;
            Entry.SetTargets(f.Self.transform, f.Camera, new[] { f.Self.transform, null, null }, new[] { f.Self }, new[] { 0 }, true);
            f.Render();
            Require(Entry.GetStatus().Contains("captured actual lobby"), "Explicit lobby camera was ignored");
            f.EnterMatch(); f.Render();
            Require(Entry.GetStatus().Contains("active on"), "Match did not compose after explicit source");
            typeof(Entry).GetField("lastCompositeTime", flags).SetValue(null, Time.realtimeSinceStartup - 5);
            Require(!Entry.GetStatus().Contains("active on"), "Stale composition was reported active");
            Entry.SetTargets(f.Self.transform, f.Camera, new[] { f.Self.transform, null, null }, new[] { f.Self }, new[] { 0 }, true);
            f.Render();
            Require(Entry.GetStatus().Contains("captured actual lobby"), "Returning to lobby failed to refresh baseline");
            report.Add("Explicit lobby camera; misleading hierarchy; expired status; lobby re-entry: PASS");
        }
    }

    static IEnumerator OwnTransparentForeground()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            yield return Frames();
            var shell = GameObject.CreatePrimitive(PrimitiveType.Quad).GetComponent<Renderer>();
            shell.name = "Replacement transparent clothing";
            shell.transform.SetParent(f.Self.transform, false);
            shell.transform.localPosition = new Vector3(0, 0, -.55f);
            shell.transform.localScale = new Vector3(.7f, .7f, 1);
            shell.gameObject.layer = 8;
            var material = new Material(Shader.Find("Universal Render Pipeline/Unlit")); f.Owned.Add(material);
            material.SetColor("_BaseColor", new Color(.5f, .2f, .1f, 1));
            material.SetFloat("_Surface", 1); material.SetFloat("_ZWrite", 0);
            material.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha); material.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha);
            material.EnableKeyword("_SURFACE_TYPE_TRANSPARENT"); material.renderQueue = 3000; shell.sharedMaterial = material;
            var hosts = new[] { f.Self.transform, null, null };
            var renderers = new[] { f.Self, shell };
            Entry.SetTargets(f.Self.transform, f.Camera, hosts, renderers, new[] { 0, 0 }, true);
            var lobby = f.Render();
            f.EnterMatch(false);
            Entry.SetTargets(null, null, hosts, renderers, new[] { 0, 0 }, true);
            var result = f.Render();
            var error = Difference(lobby, result, 44, 86, 12, 16);
            report.Add("Replacement transparent clothing over opaque body: error=" + error);
            Require(error < .003f, "Model's own transparent parts blocked lobby composition");
            Require(!shell.forceRenderingOff && shell.gameObject.layer == 8, "Clothing visibility not restored");
        }
    }

    static IEnumerator RealModel(string shader, bool srgb)
    {
        using (var f = new Fixture(true, true, false, false))
        {
            f.Self.enabled = false; f.Other.enabled = false;
            if (srgb)
            {
                var output = new RenderTexture(size, size, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
                output.Create(); f.Owned.Add(output); f.Camera.targetTexture = output;
                Require(output.sRGB, "sRGB output fixture did not enable conversion");
            }
            var doc = RealLobbyModel.Load();
            var root = new GameObject("Real Saki bind pose").transform;
            var renderers = RealLobbyModel.Create(doc, root, shader, f.Owned);
            var hosts = new[] { root, null, null }; var owners = new int[renderers.Length];
            yield return Frames();
            Entry.SetTargets(root, f.Camera, hosts, renderers, owners, true);
            var lobby = f.Render();
            Require(Entry.GetStatus().Contains("captured actual lobby"), "Real model lobby not captured");
            f.EnterMatch(false); Entry.SetTargets(null, null, hosts, renderers, owners, true);
            var result = f.Render();
            Require(Entry.GetStatus().Contains("active on"), "Real model not composed: " + Entry.GetStatus());
            var previous = RenderTexture.active; RenderTexture.active = Entry.GetModelTarget(0, "coverage");
            var mask = new Texture2D(size, size, TextureFormat.RGBAFloat, false, true); f.Owned.Add(mask);
            try { mask.ReadPixels(new Rect(0, 0, size, size), 0, 0); mask.Apply(); }
            finally { RenderTexture.active = previous; }
            var coverage = mask.GetPixels();
            Entry.SetTargets(null, null, new Transform[3], null, null, false); var original = f.Render();
            float maximum = 0, total = 0, changed = 0; int pixels = 0;
            for (int y = 1; y < size - 1; ++y) for (int x = 1; x < size - 1; ++x)
            {
                bool interior = true;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                {
                    var c = coverage[(y + dy) * size + x + dx];
                    interior &= c.r > .999f && (SystemInfo.usesReversedZBuffer ? c.g > .000001f : c.g < .999999f);
                }
                if (!interior) continue;
                float error = Difference(lobby, result, x, y, 1, 1);
                maximum = Mathf.Max(maximum, error); total += error; ++pixels;
                changed = Mathf.Max(changed, Difference(lobby, original, x, y, 1, 1));
            }
            report.Add("Real Saki shader=" + shader + " output=" + f.Camera.targetTexture.graphicsFormat +
                " parts=" + renderers.Length + " pixels=" + pixels + " max=" + maximum + " mean=" + total / Mathf.Max(1, pixels) +
                " originalDifference=" + changed + " GLB=" + doc.sha256);
            Require(pixels > 500 && changed > .02f, "Real model fixture did not exercise visible scene lighting");
            Require(maximum < (srgb ? .009f : .003f), "Real model lobby appearance differs in match");
            foreach (var renderer in renderers)
                Require(renderer.gameObject.layer == 8 && !renderer.forceRenderingOff, "Real model visibility not restored");
        }
    }

    static IEnumerator AllModels()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            var preview = GameObject.CreatePrimitive(PrimitiveType.Sphere).GetComponent<Renderer>();
            preview.name = "Profile model"; preview.gameObject.layer = 11;
            preview.transform.position = new Vector3(0, 0, 10); preview.sharedMaterial = f.Self.sharedMaterial;
            var camera = new GameObject("Profile camera").AddComponent<Camera>(); camera.CopyFrom(f.Camera);
            camera.tag = "Untagged"; camera.transform.position = new Vector3(0, 0, 5); camera.cullingMask = 1 << 11;
            camera.targetTexture = new RenderTexture(size, size, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            camera.targetTexture.Create(); f.Owned.Add(camera.targetTexture);
            var extra = camera.GetUniversalAdditionalCameraData(); extra.renderPostProcessing = true; extra.requiresDepthTexture = true;
            f.Camera.cullingMask &= ~(1 << 11);
            var replacement = GameObject.CreatePrimitive(PrimitiveType.Sphere).GetComponent<Renderer>();
            replacement.name = "Switched model"; replacement.gameObject.layer = 8;
            replacement.transform.position = f.Self.transform.position; replacement.enabled = false;
            replacement.sharedMaterial = new Material(f.Self.sharedMaterial); f.Owned.Add(replacement.sharedMaterial);
            replacement.sharedMaterial.SetColor("_BaseColor", new Color(.15f, .45f, .3f, 1));
            yield return Frames();
            var lobby = f.CaptureLobby(); var profileLobby = f.Render(camera);
            f.Self.enabled = false; replacement.enabled = true;
            var replacementLobby = f.Render();
            f.Self.enabled = true; replacement.enabled = false;
            f.EnterMatch();
            var hosts = new[] { f.Self.transform, f.Other.transform, preview.transform };
            var models = new[] { f.Self, f.Other, preview }; var owners = new[] { 0, 1, 2 };
            Entry.SetTargets(null, hosts, models, owners, false);
            var original = f.Render(); var profileOriginal = f.Render(camera);
            Entry.SetTargets(null, hosts, models, owners, true);
            var combined = f.Render();
            Require(Entry.GetStatus().Contains("active on 2 model"), "Self and Rabbit did not both render");
            var profile = f.Render(camera);
            Require(Difference(combined, lobby, 46, 78, 15, 10) < .018f, "Self missed lobby rendering with Rabbit enabled");
            Require(Difference(combined, lobby, 129, 78, 15, 25) < .018f, "Rabbit missed lobby rendering");
            Require(Difference(profile, profileLobby, 85, 78, 22, 25) < .018f, "Profile camera missed lobby rendering");
            Require(Difference(profileOriginal, profileLobby, 85, 78, 22, 25) > .02f, "Profile fixture has no lighting difference");
            f.AssertRestored();
            Entry.SetTargets(null, new Transform[] { null, f.Other.transform, null }, new[] { f.Other }, new[] { 1 }, true);
            var rabbitOnly = f.Render();
            Require(Difference(rabbitOnly, lobby, 129, 78, 15, 25) < .018f, "Rabbit depends on Self being enabled");
            Require(Difference(rabbitOnly, original, 46, 78, 15, 10) < .002f, "Disabling Self left a stale Self target");
            Entry.SetTargets(null, hosts, models, owners, false);
            Require(Difference(f.Render(), original, 0, 0, size, size) < .002f, "Switching off failed to restore all models");
            Require(Difference(f.Render(camera), profileOriginal, 0, 0, size, size) < .002f, "Switching off failed to restore Profile");
            f.Self.enabled = false; replacement.enabled = true;
            var replacementOriginal = f.Render();
            // Same host, different Renderer; then transfer it from Self to
            // Profile within the same frame without recapturing the lobby.
            Entry.SetTargets(null, new[] { f.Self.transform, null, null }, new[] { replacement }, new[] { 0 }, true);
            Require(Difference(f.Render(), replacementLobby, 46, 78, 15, 6) < .018f, "Model switch retained stale renderers");
            Entry.SetTargets(null, new[] { (Transform)null, null, f.Self.transform }, new[] { replacement }, new[] { 2 }, true);
            Require(Difference(f.Render(), replacementLobby, 46, 78, 15, 6) < .018f, "Self/Profile handoff lost lobby rendering");
            Entry.SetTargets(null, new Transform[3], null, null, false);
            Require(Difference(f.Render(), replacementOriginal, 0, 0, size, size) < .002f, "Clear after handoff left a stale composite");
            replacement.enabled = false; f.Self.enabled = true;
            camera.targetTexture = null;
            report.Add("Self + Rabbit + separate Profile camera; Rabbit alone; model switch/handoff; option off: PASS");
        }
    }

    static IEnumerator ProfileOnUICamera()
    {
        using (var f = new Fixture(true, true, false, true))
        {
            yield return Frames();
            var lobby = f.CaptureLobby(); f.EnterMatch();
            f.Camera.cullingMask &= ~(1 << 8);
            f.UI.cullingMask |= 1 << 8;
            var hosts = new Transform[] { null, null, f.Self.transform };
            Entry.SetTargets(null, hosts, new[] { f.Self }, new[] { 2 }, false);
            var original = f.Render();
            Entry.SetTargets(null, hosts, new[] { f.Self }, new[] { 2 }, true);
            var result = f.Render();
            Require(Entry.GetStatus().Contains("active on 1 model"), "UI-camera Profile did not compose: " + Entry.GetStatus());
            float modelError = Difference(result, lobby, 46, 78, 15, 6);
            Require(Difference(original, lobby, 46, 78, 15, 6) > .02f, "UI-camera fixture has no lighting difference");
            Require(modelError < .018f, "UI-camera Profile missed lobby rendering: " + modelError);
            int uiPixels = 0; float uiError = 0;
            for (int y = 55; y < 137; ++y) for (int x = 26; x < 79; ++x)
            {
                var color = original[y * size + x];
                if (color.g < .15f || color.g < color.r * 4 || color.g < color.b * 2) continue;
                ++uiPixels; uiError = Mathf.Max(uiError, Difference(result, original, x, y, 1, 1));
            }
            Require(uiPixels > 50 && uiError < .002f, "Profile composition changed Canvas UI");
            float worldError = Difference(result, original, 129, 78, 15, 25);
            report.Add($"UI-camera Profile values: model={modelError:G7} UI={uiError:G7} world={worldError:G7}");
            Require(worldError < .002f, "Profile changed the world camera model");
            f.AssertRestored();
            Entry.SetTargets(null, hosts, new[] { f.Self }, new[] { 2 }, false);
            Require(Difference(f.Render(), original, 0, 0, size, size) < .002f, "UI-camera Profile did not restore on disable");
            report.Add($"Profile on UI camera: model={modelError:G7} UI={uiError:G7} pixels={uiPixels}; off: PASS");
        }
    }

    static IEnumerator Multisampling()
    {
        using (var f = new Fixture(true, false, false, false))
        {
            f.Pipeline.msaaSampleCount = 4; f.Camera.allowMSAA = true;
            var target = f.Camera.targetTexture; target.Release(); target.antiAliasing = 4; target.Create();
            yield return Frames();
            var lobby = f.CaptureLobby(); f.EnterMatch();
            f.Pipeline.msaaSampleCount = 1; f.Camera.allowMSAA = false;
            target.Release(); target.antiAliasing = 1; target.Create();
            var result = f.Render();
            var privateColor = Entry.GetModelTarget(0, "colorA");
            Require(privateColor && privateColor.antiAliasing > 1, "Captured lobby MSAA was discarded");
            float error = Difference(result, lobby, 46, 78, 15, 6);
            report.Add("MSAA samples=" + privateColor.antiAliasing + " error=" + error.ToString("G7") +
                " lobby=" + lobby[82 * size + 52].ToString("G7") + " result=" + result[82 * size + 52].ToString("G7") +
                " coverage=" + SampleTarget("coverage", 52, 82).ToString("G7"));
            Require(error < .018f, "MSAA lobby model differs");
            Require(f.Pipeline.msaaSampleCount == 1 && !f.Camera.allowMSAA, "Lobby MSAA changed the match camera");
            f.AssertRestored();
            report.Add("Captured MSAA=" + privateColor.antiAliasing + "; model and match restoration: PASS");
        }
    }

    static IEnumerator RendererFeatures()
    {
        using (var f = new Fixture(true, false, false, false))
        {
            var feature = ScriptableObject.CreateInstance<LobbyFixtureFeature>(); f.Owned.Add(feature);
            feature.Tint = new Material(AssetDatabase.LoadAssetAtPath<Shader>("Assets/Editor/LobbyFixture.shader"));
            f.Owned.Add(feature.Tint);
            feature.Tint.SetColor("_Tint", new Color(.2f, .8f, .4f, 1));
            f.RendererData.rendererFeatures.Add(feature);
            yield return Frames();
            f.Render();
            var sourceMaterials = new HashSet<Material>(LobbyFixtureFeature.Generated);
            foreach (var material in sourceMaterials) f.Owned.Add(material);
            var lobby = f.CaptureLobby();
            // Replacing an unused snapshot must dispose the clone created by
            // OnEnable, even before a private ScriptableRenderer is built.
            for (int i = 0; i < 3; ++i)
            {
                typeof(Entry).GetField("nextCapture", flags).SetValue(null, 0f);
                f.Render();
                yield return Frames();
                Require(LobbyFixtureFeature.Live.Count == 2, "Unused captured renderer feature leaked");
                int livePrivate = 0;
                foreach (var material in LobbyFixtureFeature.Generated)
                    if (material && !sourceMaterials.Contains(material)) ++livePrivate;
                Require(livePrivate == 1, "Generated pass Material leaked when an unused snapshot was replaced");
            }
            var captured = (IList)typeof(Entry).GetField("views", flags).GetValue(null);
            var capturedData = (UniversalRendererData)captured[0].GetType().GetField("rendererData", flags).GetValue(captured[0]);
            var clone = (LobbyFixtureFeature)capturedData.rendererFeatures[0];
            Require(clone != feature && clone.Tint != feature.Tint, "Captured feature shares its source Material");
            feature.Tint.SetColor("_Tint", Color.white); feature.SetActive(false);
            Entry.SetTargets(null, new[] { f.Self.transform, f.Other.transform, null }, new[] { f.Self, f.Other }, new[] { 0, 1 }, false);
            var original = f.Render();
            Entry.SetTargets(null, new[] { f.Self.transform, f.Other.transform, null }, new[] { f.Self, f.Other }, new[] { 0, 1 }, true);
            var result = f.Render();
            Require(Difference(original, lobby, 46, 78, 15, 6) > .05f, "Feature fixture has no visible effect");
            Require(Difference(result, lobby, 46, 78, 15, 6) < .002f &&
                    Difference(result, lobby, 129, 78, 15, 25) < .002f, "Captured active feature missing from a model");
            Require(!feature.isActive && feature.Tint.GetColor("_Tint") == Color.white, "Source renderer feature was modified");
            Entry.Shutdown();
            Require(LobbyFixtureFeature.Live.Count == 1 && LobbyFixtureFeature.Live.Contains(feature.GetInstanceID()), "Captured feature leaked after shutdown");
            yield return Frames();
            foreach (var material in LobbyFixtureFeature.Generated)
                Require((bool)material == sourceMaterials.Contains(material), "Generated pass Material ownership was not preserved");
            feature.Dispose();
            LobbyFixtureFeature.Generated.Clear();
            report.Add("Active renderer feature on two models; Material isolation; repeated Create/unused/used resource cleanup: PASS");
        }
    }

    static void RotateProbe()
    {
        var sh = new SphericalHarmonicsL2(); sh.AddAmbientLight(new Color(.6f, .5f, .4f));
        sh.AddDirectionalLight(new Vector3(1, 2, 3).normalized, new Color(.4f, .2f, .1f), .5f);
        var rotation = Quaternion.Euler(20, 75, 15); var transformed = Harmonics.Rotate(sh, rotation);
        var directions = new Vector3[32]; var rotatedDirections = new Vector3[32];
        for (int i = 0; i < 32; ++i)
        {
            directions[i] = new Vector3(Mathf.Sin(i * 1.73f), Mathf.Cos(i * .51f), Mathf.Cos(i * 2.23f)).normalized;
            rotatedDirections[i] = Quaternion.Inverse(rotation) * directions[i];
        }
        var expected = new Color[32]; var actual = new Color[32]; sh.Evaluate(rotatedDirections, expected); transformed.Evaluate(directions, actual);
        float error = 0;
        for (int i = 0; i < 32; ++i) error = Mathf.Max(error, Mathf.Abs(expected[i].r - actual[i].r), Mathf.Abs(expected[i].g - actual[i].g), Mathf.Abs(expected[i].b - actual[i].b));
        report.Add("SH rotation error=" + error.ToString("G7")); Require(error < 1e-4f, "Rotated probe lighting changed");
    }

    static IEnumerator LayerFeaturesAndDynamicObjects()
    {
        using (var f = new Fixture(false, false, false, false))
        {
            var feature = ScriptableObject.CreateInstance<LobbyFixtureFeature>(); f.Owned.Add(feature);
            feature.GeometryOnly = true; feature.Layers = 1 << 8;
            feature.Tint = new Material(Shader.Find("Hidden/FTRez/LobbyFixtureTint")); f.Owned.Add(feature.Tint);
            feature.Tint.SetColor("_Tint", new Color(.15f, .9f, .35f, 1));
            f.RendererData.rendererFeatures.Add(feature);
            f.Self.probeAnchor = f.Other.probeAnchor = f.Self.transform;
            yield return Frames();
            var hosts = new[] { f.Self.transform, null, null };
            var pieces = new[] { f.Self, f.Other }; var owners = new[] { 0, 0 };
            f.Render(); Entry.SetTargets(f.Self.transform, f.Camera, hosts, pieces, owners, true);
            var lobby = f.Render(); f.EnterMatch(false); feature.SetActive(false);
            Entry.SetTargets(null, null, hosts, pieces, owners, true);
            var result = f.Render();
            Require(Difference(result, lobby, 46, 78, 15, 6) < .002f, "Layer-filtered lobby feature was lost");
            Require(Difference(result, lobby, 129, 78, 15, 25) < .002f, "Layer-filtered feature affected a different model part sharing the same probe anchor");
            Entry.SetTarget(f.Self.transform, new[] { f.Self }, true);
            var foreign = GameObject.CreatePrimitive(PrimitiveType.Cube); foreign.layer = 30;
            foreign.transform.position = new Vector3(.7f, 0, -.4f);
            var extraLight = new GameObject("Dynamic world light").AddComponent<Light>();
            extraLight.type = LightType.Directional; extraLight.color = Color.magenta; extraLight.intensity = 7;
            yield return Frames();
            var changed = f.Render();
            Require(Difference(changed, lobby, 46, 78, 15, 6) < .002f, "Dynamic layer/light leaked into lobby model");
            Entry.SetTarget(null, null, true); var original = f.Render();
            Require(Difference(changed, original, 129, 78, 15, 25) < .002f, "Dynamic world geometry was replaced");
            Require(extraLight.enabled && foreign.layer == 30 && feature.Layers == (1 << 8), "World layer/light/feature changed");
            report.Add("Layer-filtered feature, newly spawned occupied layer and strong world light: PASS");
            Entry.Shutdown(); feature.Dispose();
            foreach (var material in LobbyFixtureFeature.Generated) if (material) f.Owned.Add(material);
            LobbyFixtureFeature.Generated.Clear();
        }
    }

    static IEnumerator CroppedModel(bool orthographic)
    {
        using (var f = new Fixture(true, true, false, true))
        {
            const int width = 1024;
            var output = new RenderTexture(width, width, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            output.Create(); f.Owned.Add(output); f.Camera.targetTexture = output; f.UI.targetTexture = output;
            f.Camera.orthographic = orthographic; f.Camera.orthographicSize = 1.7f;
            f.UI.orthographic = orthographic; f.UI.orthographicSize = 1.7f;
            yield return Frames();
            var lobby = f.CaptureLobby(); f.EnterMatch(); var result = f.Render();
            var product = Entry.GetModelTarget(0, "colorA");
            int privateWidth = product.width, privateHeight = product.height;
            Require(product.width * product.height < width * width / 2, "Model crop did not reduce private fill cost");
            float error = 0;
            int centerX = orthographic ? 302 : 280, centerY = 436;
            for (int y = centerY - 10; y < centerY + 10; ++y) for (int x = centerX - 10; x < centerX + 10; ++x)
            {
                var d = lobby[y * width + x] - result[y * width + x];
                error = Mathf.Max(error, Mathf.Abs(d.r), Mathf.Abs(d.g), Mathf.Abs(d.b));
            }
            Require(error < .003f, "Cropped model color/depth alignment differs: " + error);
            Entry.SetTarget(null, null, true); var original = f.Render();
            float other = 0;
            for (int y = 0; y < width; ++y) for (int x = width / 2; x < width; ++x)
            {
                var d = original[y * width + x] - result[y * width + x];
                other = Mathf.Max(other, Mathf.Abs(d.r), Mathf.Abs(d.g), Mathf.Abs(d.b));
            }
            Require(other < .002f, "Cropped composition changed unrelated pixels");
            report.Add("Native-density crop orthographic=" + orthographic + " private=" + privateWidth + "x" + privateHeight +
                " world=1024x1024 modelError=" + error + " otherError=" + other + ": PASS");
        }
    }

    static void CropHysteresis()
    {
        var sizing = new CropSizing();
        int w = 320, h = 576;
        sizing.Fit(ref w, ref h, 1600, 900, 10);
        Require(w == 1600 && h == 900, "Crop shrank without grace period");
        w = 384; h = 576; sizing.Fit(ref w, ref h, 1600, 900, 10.4f);
        w = 320; h = 576; sizing.Fit(ref w, ref h, 1600, 900, 10.8f);
        Require(w == 384 && h == 576, "Full-screen crop did not recover to peak demand");
        w = 320; h = 576; sizing.Fit(ref w, ref h, 384, 576, 11);
        Require(w == 384 && h == 576, "One allocation bucket caused resize churn");
        w = 512; h = 640; sizing.Fit(ref w, ref h, 384, 576, 11.1f);
        Require(w == 512 && h == 640, "Crop growth clipped newly visible pixels");
        report.Add("Crop hysteresis: sustained shrink, peak demand, jitter retention, immediate growth: PASS");
    }

    static IEnumerator CropRecovery()
    {
        using (var f = new Fixture(true, true, false, true))
        {
            var output = new RenderTexture(1024, 1024, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            output.Create(); f.Owned.Add(output); f.Camera.targetTexture = output; f.UI.targetTexture = output;
            yield return Frames();
            var lobby = f.CaptureLobby(); f.EnterMatch(); f.Render();
            var fields = typeof(Entry).GetField("targets", BindingFlags.Static | BindingFlags.NonPublic);
            var target = ((Array)fields.GetValue(null)).GetValue(0);
            var originalField = target.GetType().GetField("Original");
            var original = originalField.GetValue(target);
            var before = Entry.GetModelTarget(0, "colorA"); int initialArea = before.width * before.height;
            var position = f.Self.transform.position;
            f.Self.transform.position = f.Camera.transform.position + f.Camera.transform.forward * .35f;
            f.Render();
            var near = Entry.GetModelTarget(0, "colorA"); int nearArea = near.width * near.height;
            Require(nearArea > initialArea, "Near-clip fixture failed to enlarge private target");
            f.Self.transform.position = position; f.Render();
            float deadline = Time.realtimeSinceStartup + .85f;
            while (Time.realtimeSinceStartup < deadline) yield return null;
            var recovered = f.Render(); var after = Entry.GetModelTarget(0, "colorA");
            Require(after.width * after.height == initialArea, "Near-clip target remained oversized after moving away");
            Require(ReferenceEquals(before, after), "Returning crop reallocated its retained render target");
            Require(ReferenceEquals(original, originalField.GetValue(target)), "RT resize rebound the original model");
            float error = 0;
            for (int y = 426; y < 446; ++y) for (int x = 270; x < 290; ++x)
            { var d = lobby[y * 1024 + x] - recovered[y * 1024 + x]; error = Mathf.Max(error, Mathf.Abs(d.r), Mathf.Abs(d.g), Mathf.Abs(d.b)); }
            Require(error < .003f, "Recovered crop changed model pixels: " + error);
            report.Add("Near-clip -> move away: area=" + initialArea + " -> " + nearArea + " -> " + (after.width * after.height) +
                " original retained, modelError=" + error + ": PASS");
        }
    }

    static void RenderBufferReuse()
    {
        using (var f = new Fixture(true, true, true, false))
        {
            var all = (Array)typeof(Entry).GetField("targets", flags).GetValue(null);
            var target = all.GetValue(0); var second = all.GetValue(1);
            var resize = typeof(Entry).GetMethod("ResizeBuffers", flags);
            var type = target.GetType();
            var spareField = type.GetField("Spare");
            var owned = new HashSet<RenderTexture>();
            Action<object, int, int, UnityEngine.Experimental.Rendering.GraphicsFormat, int, bool> fit = (t, w, h, format, samples, later) => {
                resize.Invoke(null, new object[] { t, w, h, format, samples, later });
                foreach (var name in new[] { "ColorA", "ColorB", "Coverage" })
                { var value = (RenderTexture)type.GetField(name).GetValue(t); if (value) owned.Add(value); }
                var spares = (IList)spareField.GetValue(t); long bytes = 0;
                foreach (var spare in spares) bytes += (long)spare.GetType().GetField("Bytes").GetValue(spare);
                Require(spares.Count <= 2 && bytes <= 16L * 1024 * 1024, "Spare RT budget exceeded");
            };
            var hdr = UnityEngine.Experimental.Rendering.GraphicsFormat.R16G16B16A16_SFloat;
            fit(target, 320, 384, hdr, 1, true);
            var first = Entry.GetModelTarget(0, "colorA"); var firstB = Entry.GetModelTarget(0, "colorB");
            var coverage = Entry.GetModelTarget(0, "coverage");
            var composite = type.GetField("Composite").GetValue(target);
            for (int i = 0; i < 12; ++i)
            {
                fit(target, 448, 384, hdr, 1, true); fit(target, 320, 384, hdr, 1, true);
                Require(ReferenceEquals(first, Entry.GetModelTarget(0, "colorA")) &&
                    ReferenceEquals(firstB, Entry.GetModelTarget(0, "colorB")) &&
                    ReferenceEquals(coverage, Entry.GetModelTarget(0, "coverage")), "Stable crop cycle allocated new RTs");
                Require(ReferenceEquals(composite, type.GetField("Composite").GetValue(target)), "Resize recreated composite material");
            }
            fit(second, 320, 384, hdr, 1, true);
            Require(!ReferenceEquals(first, Entry.GetModelTarget(1, "colorA")), "Different slots share writable RTs");
            fit(target, 448, 384, hdr, 1, true); first.Release();
            fit(target, 320, 384, hdr, 1, true);
            Require(!ReferenceEquals(first, Entry.GetModelTarget(0, "colorA")), "Lost RT storage was reused");
            var ldr = UnityEngine.Experimental.Rendering.GraphicsFormat.R8G8B8A8_UNorm;
            fit(target, 320, 384, ldr, 1, false);
            Require(Entry.GetModelTarget(0, "colorA").graphicsFormat == ldr && !Entry.GetModelTarget(0, "colorB"), "Format/stage change reused incompatible buffers");
            fit(target, 320, 384, hdr, 1, true);
            Require(Entry.GetModelTarget(0, "colorB"), "Later camera stage lost its target");
            fit(target, 1600, 900, hdr, 1, true); var large = Entry.GetModelTarget(0, "colorA");
            fit(target, 320, 384, hdr, 1, true);
            Require(!large || !large.IsCreated(), "Oversized RT remained cached");
            Entry.Shutdown();
            foreach (var texture in owned) Require(!texture || !texture.IsCreated(), "Shutdown leaked active/spare RTs");
            report.Add("Render buffers: 12 resize cycles reuse A/B/depth + material, slot isolation, loss/format/stage recovery, bounded retention, shutdown: PASS");
        }
    }

    static void OriginalRestoration()
    {
        var scene = SceneManager.GetActiveScene();
        var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
        var source = go.GetComponent<Renderer>();
        var first = new Material(Shader.Find("Universal Render Pipeline/Lit"));
        var second = new Material(first);
        source.sharedMaterials = new[] { first, first };
        int identity = source.GetInstanceID();
        var layer = go.layer;
        var isolation = new LayerIsolation();
        var privateScene = SceneManager.CreateScene("Original borrow validation");
        try
        {
            using (var model = new OriginalModel(new[] { source }))
            {
                Require(ReferenceEquals(model.Renderers[0], source), "Model renderer was duplicated");
                var block = new MaterialPropertyBlock(); int key = Shader.PropertyToID("_BaseColor");
                block.SetColor(key, Color.red); source.SetPropertyBlock(block, 1);
                block.SetColor(key, Color.blue); source.SetPropertyBlock(block);
                var mesh = go.GetComponent<MeshFilter>().sharedMesh;
                for (int pass = 0; pass < 3; ++pass)
                {
                    isolation.Refresh(1 << layer, privateScene);
                    model.Borrow(isolation);
                    try
                    {
                        isolation.SetLayer(go, isolation.Layer(layer));
                        model.PrepareLighting(0);
                        block.SetColor(key, Color.green);
                        source.SetPropertyBlock(block); source.SetPropertyBlock(block, 1);
                        if (pass == 1) throw new InvalidOperationException("synthetic render failure");
                    }
                    catch (InvalidOperationException error)
                    { Require(error.Message == "synthetic render failure", "Unexpected borrow failure"); }
                    finally { model.Restore(); }
                    source.GetPropertyBlock(block);
                    Require(block.GetColor(key) == Color.blue, "Renderer MPB was not restored");
                    source.GetPropertyBlock(block, 1);
                    Require(block.GetColor(key) == Color.red, "Indexed MPB was not restored");
                    Require(go.layer == layer && go.scene == scene && source.lightProbeUsage == LightProbeUsage.BlendProbes,
                        "Borrow changed layer, scene or probe usage");
                    Require(source.GetInstanceID() == identity && go.GetComponent<MeshFilter>().sharedMesh == mesh,
                        "Original renderer/mesh identity changed");
                }
                source.SetPropertyBlock(null); source.SetPropertyBlock(null, 1);
                source.sharedMaterials = new[] { second };
                model.Borrow(isolation); model.PrepareLighting(0); source.SetPropertyBlock(block); model.Restore();
                Require(!source.HasPropertyBlock(), "Empty MPB was not restored after material shrink");
                source.sharedMaterials = new[] { second, first };
                model.Borrow(isolation); model.Restore();
                source.GetPropertyBlock(block, 1);
                Require(block.isEmpty, "Removed indexed MPB reappeared after material regrow");
                Require(source.sharedMaterials[0] == second, "Original live material changed");
            }
            Require(source && source.enabled, "Disposal destroyed or hid the original model");
        }
        finally
        {
            Object.DestroyImmediate(go); Object.DestroyImmediate(first); Object.DestroyImmediate(second);
            SceneManager.UnloadSceneAsync(privateScene);
        }
        report.Add("Direct original: identity/scene retained, MPBs restored after success/failure, slot shrink/regrow, disposal: PASS");
    }

    static Cubemap SolidCube(Color color, List<Object> owned)
    {
        var cube = new Cubemap(16, TextureFormat.RGBAHalf, false); owned.Add(cube);
        var pixels = new Color[256]; for (int i = 0; i < pixels.Length; ++i) pixels[i] = color;
        for (int face = 0; face < 6; ++face) cube.SetPixels(pixels, (CubemapFace)face);
        cube.Apply(); return cube;
    }

    static IEnumerator EmptyReflectionProbe()
    {
        using (var f = new Fixture(true, false, false, false))
        {
            f.Self.sharedMaterial.SetFloat("_Metallic", 1);
            f.Self.sharedMaterial.SetFloat("_Smoothness", .8f);
            RenderSettings.defaultReflectionMode = DefaultReflectionMode.Custom;
            RenderSettings.customReflectionTexture = SolidCube(new Color(.6f, .2f, .03f, 1), f.Owned);
            RenderSettings.reflectionIntensity = .65f;
            var probe = new GameObject("Unbaked lobby probe").AddComponent<ReflectionProbe>();
            probe.mode = ReflectionProbeMode.Baked;
            probe.transform.position = f.Self.transform.position; probe.size = Vector3.one * 4;
            yield return Frames(); var lobby = f.CaptureLobby();
            f.EnterMatch();
            RenderSettings.customReflectionTexture = SolidCube(new Color(.02f, .3f, 1, 1), f.Owned);
            var result = f.Render();
            float error = Difference(result, lobby, 46, 78, 15, 6);
            report.Add("Unbaked probe and default reflection fallback: error=" + error);
            Require(error < .003f, "Unbaked reflection probe changed character material");
        }
    }

    static IEnumerator OpaqueMaterialAlpha(bool msaa, bool cutout = false, bool packed = false)
    {
        using (var f = new Fixture(true, false, false, false))
        {
            var shader = Shader.Find("FTREZ/SakiSource");
            Require(shader, "Real Saki shader required for opaque alpha regression");
            var material = new Material(shader); f.Owned.Add(material);
            material.SetColor("_BaseColor", new Color(.7f, .6f, .5f, .35f));
            material.SetFloat("_SurfaceKind", 0);
            f.Self.sharedMaterial = material;
            if (cutout)
            {
                var texture = new Texture2D(8, 8, TextureFormat.RGBA32, false, true); f.Owned.Add(texture);
                texture.filterMode = FilterMode.Point;
                for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
                    texture.SetPixel(x, y, new Color(1, 1, 1, (x + y) % 2));
                texture.Apply(); material.SetTexture("_BaseMap", texture);
                material.SetFloat("_AlphaClip", 1); material.SetFloat("_Cutoff", .1f);
                material.SetFloat("_Cull", 2);
            }
            if (msaa)
            {
                f.Pipeline.msaaSampleCount = 4; f.Camera.allowMSAA = true;
                var target = new RenderTexture(size, size, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
                target.antiAliasing = 4; target.Create(); f.Owned.Add(target); f.Camera.targetTexture = target;
            }
            if (packed)
            {
                var target = new RenderTexture(size, size, 24, RenderTextureFormat.RGB111110Float, RenderTextureReadWrite.Linear);
                target.Create(); f.Owned.Add(target); f.Camera.targetTexture = target;
            }
            yield return Frames(); var lobby = f.CaptureLobby(); f.EnterMatch();
            var result = f.Render();
            if (packed) Require(Entry.GetModelTarget(0, "colorA").graphicsFormat == UnityEngine.Experimental.Rendering.GraphicsFormat.R16G16B16A16_SFloat,
                "Packed HDR compatibility format changed");
            var coverage = Entry.GetModelTarget(0, "coverage");
            var previous = RenderTexture.active; RenderTexture.active = coverage;
            var mask = new Texture2D(size, size, TextureFormat.RGBAFloat, false, true); f.Owned.Add(mask);
            try { mask.ReadPixels(new Rect(0, 0, size, size), 0, 0); mask.Apply(); }
            finally { RenderTexture.active = previous; }
            Entry.SetTarget(null, null, true); var world = f.Render();
            float error = 0, changed = 0; int covered = 0, holes = 0;
            for (int y = 78; y < 96; ++y) for (int x = 46; x < 70; ++x)
            {
                if (cutout && mask.GetPixel(x, y).r < .999f)
                {
                    ++holes;
                    Require(Difference(result, world, x, y, 1, 1) < .002f, "Cutout hole replaced world pixel");
                    continue;
                }
                ++covered;
                error = Mathf.Max(error, Difference(result, lobby, x, y, 1, 1));
                changed = Mathf.Max(changed, Difference(world, lobby, x, y, 1, 1));
            }
            Require(!cutout || (covered > 10 && holes > 10), "Cutout fixture lacks covered pixels or holes");
            report.Add("Opaque Saki alpha .35 MSAA=" + msaa + " cutout=" + cutout + " packed=" + packed + " error=" + error +
                " originalDifference=" + changed + " covered=" + covered + " holes=" + holes);
            Require(changed > .01f, "Opaque alpha fixture does not exercise lighting");
            Require(error < .003f, "Opaque material alpha prevented lobby replacement");
            Require(Difference(result, world, 129, 78, 15, 25) < .002f, "Opaque alpha changed world pixels");
        }
    }

    static IEnumerator PerPartLighting()
    {
        using (var f = new Fixture(true, false, false, false))
        {
            var host = new GameObject("Multi-part character").transform;
            f.Self.transform.SetParent(host, true); f.Other.transform.SetParent(host, true);
            var parts = new[] { f.Self, f.Other };
            f.Light.intensity = .1f;
            f.Self.sharedMaterial.SetColor("_BaseColor", Color.white);
            for (int i = 0; i < parts.Length; ++i)
            {
                parts[i].lightProbeUsage = LightProbeUsage.CustomProvided;
                var sh = new SphericalHarmonicsL2();
                sh.AddAmbientLight(i == 0 ? new Color(.8f, .07f, .02f) : new Color(.02f, .15f, .9f));
                var block = new MaterialPropertyBlock();
                block.CopySHCoefficientArraysFrom(new[] { sh });
                block.CopyProbeOcclusionArrayFrom(new[] { new Vector4(.2f, .4f, .6f, .8f) });
                if (i == 0) parts[i].SetPropertyBlock(block);
                else parts[i].SetPropertyBlock(block, 0);
            }
            yield return Frames(); f.Render();
            Entry.SetTargets(host, f.Camera, new Transform[] { host, null, null }, parts, new[] { 0, 0 }, true);
            var lobby = f.Render();
            Require(Entry.GetStatus().Contains("captured actual lobby"), "Part lighting capture failed");
            f.EnterMatch(false);
            Entry.SetTargets(null, null, new Transform[] { host, null, null }, parts, new[] { 0, 0 }, true);
            var result = f.Render();
            float first = Difference(result, lobby, 46, 78, 15, 6);
            float second = Difference(result, lobby, 129, 78, 15, 6);
            report.Add("Custom lighting per part, renderer and indexed MPB: errors=" + first + "," + second);
            Require(first < .003f && second < .003f, "Custom part lighting was overwritten");
        }
    }

    static IEnumerator ReflectionAcrossPositions()
    {
        using (var f = new Fixture(true, false, false, false))
        {
            f.Self.sharedMaterial.SetFloat("_Metallic", 1); f.Self.sharedMaterial.SetFloat("_Smoothness", .8f);
            var warm = SolidCube(new Color(.6f, .2f, .03f, 1), f.Owned);
            var cold = SolidCube(new Color(.02f, .3f, 1, 1), f.Owned);
            RenderSettings.defaultReflectionMode = DefaultReflectionMode.Custom;
            RenderSettings.customReflectionTexture = warm; RenderSettings.reflectionIntensity = .65f;
            var probe = new GameObject("Lobby reflection").AddComponent<ReflectionProbe>();
            probe.transform.position = f.Self.transform.position; probe.size = Vector3.one * 4;
            probe.mode = ReflectionProbeMode.Custom; probe.customBakedTexture = warm; probe.importance = 5; probe.intensity = 1.7f;
            yield return Frames(); var lobby = f.CaptureLobby();
            // A realtime/sky reflection can reuse its texture object while the
            // game changes its pixels. The baseline must own frozen contents.
            var changedPixels = new Color[256];
            for (int i = 0; i < changedPixels.Length; ++i) changedPixels[i] = new Color(.02f, .3f, 1, 1);
            for (int face = 0; face < 6; ++face) warm.SetPixels(changedPixels, (CubemapFace)face);
            warm.Apply();
            f.EnterMatch(); probe.customBakedTexture = cold; RenderSettings.customReflectionTexture = cold;
            var result = f.Render();
            float reflectionError = Difference(result, lobby, 46, 78, 15, 6);
            report.Add("Reflection intensity fixture: error=" + reflectionError + " lobby=" + lobby[82 * size + 52] + " match=" + result[82 * size + 52]);
            Require(reflectionError < .003f, "World reflection probe changed model material");
            Entry.SetTarget(null, null, true);
            var world = f.Render();
            Require(Difference(result, world, 129, 78, 15, 25) < .002f, "Private reflection leaked into world rendering");
            Entry.SetTarget(f.Self.transform, new[] { f.Self }, true);
            var move = new Vector3(12, 3, 0);
            f.Self.transform.position += move; f.Camera.transform.position += move;
            var spawned = new List<ReflectionProbe>();
            for (int i = 0; i < 4; ++i)
            {
                var p = new GameObject("New world reflection " + i).AddComponent<ReflectionProbe>();
                p.transform.position = f.Self.transform.position; p.size = Vector3.one * 5;
                p.mode = ReflectionProbeMode.Custom; p.customBakedTexture = cold; p.importance = 20 + i; spawned.Add(p);
            }
            yield return Frames(); var moved = f.Render();
            float error = Difference(moved, lobby, 46, 78, 15, 6);
            Require(error < .003f, "Moving into different reflection probes changed lobby material: " + error);
            Require(RenderSettings.customReflectionTexture == cold && probe.enabled, "World reflection settings leaked");
            foreach (var p in spawned) Require(p.enabled && p.customBakedTexture == cold, "World probe state leaked");
            report.Add("Metallic material, distinct intensities, frozen textures, world restoration, movement and four newly spawned probes: error=" + error + " PASS");
        }
    }

    static IEnumerator SkinnedMotion()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            var source = f.Self; source.enabled = false;
            var mesh = Object.Instantiate(source.GetComponent<MeshFilter>().sharedMesh); f.Owned.Add(mesh);
            var root = new GameObject("Animated model").transform; root.position = source.transform.position;
            root.SetParent(f.Camera.transform.parent, true);
            var bone = new GameObject("Animated bone").transform; bone.SetParent(root, false);
            var go = new GameObject("Skin"); go.transform.SetParent(root, false); go.layer = 8;
            var skin = go.AddComponent<SkinnedMeshRenderer>(); skin.sharedMesh = mesh; skin.sharedMaterial = source.sharedMaterial;
            var weights = new BoneWeight[mesh.vertexCount];
            for (int i = 0; i < weights.Length; ++i) weights[i] = new BoneWeight { boneIndex0 = 0, weight0 = 1 };
            mesh.boneWeights = weights; mesh.bindposes = new[] { Matrix4x4.identity }; skin.bones = new[] { bone }; skin.rootBone = bone;
            skin.localBounds = new Bounds(Vector3.zero, Vector3.one * 3); skin.updateWhenOffscreen = true;
            f.Self = skin;
            var moved = new Vector3(.12f, .03f, 0); bone.localPosition = moved;
            yield return Frames(); var lobby = f.CaptureLobby(); f.EnterMatch();
            bone.localPosition = Vector3.zero; var initial = f.Render();
            bone.localPosition = moved; yield return Frames(); var animated = f.Render();
            float error = Difference(animated, lobby, 50, 75, 15, 10);
            Require(error < .003f && Difference(animated, initial, 45, 70, 25, 20) > .01f, "Original model did not render live skin bones: " + error);
            Require(skin.bones[0] == bone && skin.gameObject.scene == root.gameObject.scene && skin.gameObject.layer == 8, "Source skin was moved or rebound");
            report.Add("Live skinned mesh bone movement and original rig preservation: error=" + error + " PASS");
        }
    }

    static IEnumerator SharedModelInstances()
    {
        using (var f = new Fixture(true, true, false, false))
        {
            var mesh = Object.Instantiate(f.Self.GetComponent<MeshFilter>().sharedMesh); f.Owned.Add(mesh);
            var material = f.Self.sharedMaterial;
            var weights = new BoneWeight[mesh.vertexCount];
            for (int i = 0; i < weights.Length; ++i)
                weights[i] = new BoneWeight { boneIndex0 = 0, weight0 = 1 };
            mesh.boneWeights = weights; mesh.bindposes = new[] { Matrix4x4.identity };
            var bones = new Transform[2]; var skins = new SkinnedMeshRenderer[2];
            var sources = new[] { f.Self, f.Other };
            for (int i = 0; i < skins.Length; ++i)
            {
                var root = new GameObject("Shared model instance " + i).transform;
                root.SetParent(f.Camera.transform.parent, false); root.position = sources[i].transform.position;
                bones[i] = new GameObject("Independent bone " + i).transform; bones[i].SetParent(root, false);
                var go = new GameObject("Shared skin " + i); go.transform.SetParent(root, false); go.layer = 8 + i;
                skins[i] = go.AddComponent<SkinnedMeshRenderer>(); skins[i].sharedMesh = mesh;
                skins[i].sharedMaterial = material; skins[i].bones = new[] { bones[i] }; skins[i].rootBone = bones[i];
                skins[i].localBounds = new Bounds(Vector3.zero, Vector3.one * 3); skins[i].updateWhenOffscreen = true;
                sources[i].enabled = false;
            }
            f.Self = skins[0]; f.Other = skins[1];
            yield return Frames(); var lobby = f.CaptureLobby(); f.EnterMatch(false);
            Entry.SetTargets(null, null, new[] { skins[0].transform, skins[1].transform, null },
                new Renderer[] { skins[0], skins[1] }, new[] { 0, 1 }, true);
            var initial = f.Render();
            Require(Difference(initial, lobby, 50, 75, 15, 10) < .003f &&
                Difference(initial, lobby, 129, 78, 15, 20) < .003f, "Shared instances lost lobby appearance");
            bones[0].localPosition = new Vector3(.15f, .04f, 0);
            yield return Frames(); var movedSelf = f.Render();
            Require(Difference(movedSelf, initial, 40, 65, 40, 55) > .01f &&
                Difference(movedSelf, initial, 116, 65, 40, 55) < .003f, "Self motion changed Rabbit or failed to animate");
            bones[1].localPosition = new Vector3(-.12f, -.05f, 0);
            yield return Frames(); var movedRabbit = f.Render();
            Require(Difference(movedRabbit, movedSelf, 116, 65, 40, 55) > .01f &&
                Difference(movedRabbit, movedSelf, 40, 65, 40, 55) < .003f, "Rabbit motion changed Self or failed to animate");
            Require(skins[0].sharedMesh == mesh && skins[1].sharedMesh == mesh &&
                skins[0].sharedMaterial == material && skins[1].sharedMaterial == material &&
                skins[0].bones[0] != skins[1].bones[0], "Shared assets or independent bones were replaced");
            var indexCount = mesh.GetIndexCount(0);
            var local = Object.Instantiate(mesh); f.Owned.Add(local); skins[0].sharedMesh = local;
            local.triangles = Array.Empty<int>();
            yield return Frames(); var clipped = f.Render();
            Require(mesh.GetIndexCount(0) == indexCount && skins[1].sharedMesh == mesh &&
                Difference(clipped, movedRabbit, 116, 65, 40, 55) < .003f &&
                Difference(clipped, movedRabbit, 40, 65, 40, 55) > .01f, "Local first-person clipping changed the shared model");
            report.Add("Shared mesh/material: independent Self and Rabbit GPU skinning, lobby appearance and local clipping PASS");
        }
    }

    public static void Run()
    {
        PlayerSettings.colorSpace = ColorSpace.Linear;
        Directory.CreateDirectory("Validation");
        EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        EditorSettings.enterPlayModeOptionsEnabled = true;
        EditorSettings.enterPlayModeOptions = EnterPlayModeOptions.DisableDomainReload | EnterPlayModeOptions.DisableSceneReload;
        EditorApplication.playModeStateChanged += Started;
        EditorApplication.EnterPlaymode();
    }
    static void Started(PlayModeStateChange state)
    {
        if (state != PlayModeStateChange.EnteredPlayMode) return;
        EditorApplication.playModeStateChanged -= Started;
        report.Add("Unity=" + Application.unityVersion + " Graphics=" + SystemInfo.graphicsDeviceType + " SyntheticPlayerLoop=" + Application.isPlaying + " InGame=false");
        Application.logMessageReceived += Log;
        steps.Push(propertyProbe ? PropertyProbe() : Suite());
        EditorApplication.update += Step;
        EditorApplication.QueuePlayerLoopUpdate();
    }

    static readonly Stack<IEnumerator> steps = new Stack<IEnumerator>();
    static void GeometryCache()
    {
        var address = System.Runtime.InteropServices.Marshal.AllocHGlobal(16);
        var previous = GeometryChanges.NativeState;
        var own = UnityEngine.SceneManagement.SceneManager.CreateScene("Geometry cache fixture");
        GameObject world = null;
        try
        {
            GeometryChanges.NativeState = address;
            System.Runtime.InteropServices.Marshal.WriteInt64(address, 1);
            System.Runtime.InteropServices.Marshal.WriteInt32(address, 8, 0);
            var cache = new LayerIsolation(); cache.Refresh(1 << 8, own);
            int first = cache.Layer(8);
            for (int i = 0; i < 120; ++i) cache.Refresh(1 << 8, own);
            Require(cache.RefreshCount == 1, "Unchanged epoch rescanned the world");
            world = GameObject.CreatePrimitive(PrimitiveType.Cube); world.layer = first;
            System.Runtime.InteropServices.Marshal.WriteInt64(address, 2);
            cache.Refresh(1 << 8, own);
            Require(cache.RefreshCount == 2 && cache.Layer(8) != first, "Creation/layer hook did not exclude new world geometry");
            System.Runtime.InteropServices.Marshal.WriteInt32(address, 8, 1);
            cache.Refresh(1 << 8, own); cache.Refresh(1 << 8, own);
            Require(cache.RefreshCount == 4, "Pending async integration must check each render");
            System.Runtime.InteropServices.Marshal.WriteInt32(address, 8, 0);
            cache.Refresh(1 << 8, own);
            Require(cache.RefreshCount == 4, "Completed integration did not resume caching");
            GeometryChanges.NativeState = IntPtr.Zero;
            cache.Refresh(1 << 8, own); cache.Refresh(1 << 8, own);
            Require(cache.RefreshCount == 6, "Missing native hooks must retain safe checks");
            report.Add("Geometry epochs: 120 stable renders/one query; new occupied layer; async and missing-hook fallback: PASS");
        }
        finally
        {
            GeometryChanges.NativeState = previous;
            System.Runtime.InteropServices.Marshal.FreeHGlobal(address);
            if (world) Object.DestroyImmediate(world);
            UnityEngine.SceneManagement.SceneManager.UnloadSceneAsync(own);
        }
    }
    static void CompositeScissor()
    {
        const int width = 32;
        var owned = new List<Object>();
        var previous = RenderTexture.active;
        var oldOrtho = Shader.GetGlobalVector("unity_OrthoParams");
        var oldProjection = Shader.GetGlobalVector("_ProjectionParams");
        var command = new CommandBuffer();
        try
        {
            var shader = AssetDatabase.LoadAssetAtPath<Shader>("Assets/LobbyModel/LobbyModel.shader");
            var material = new Material(shader); owned.Add(material);
            var coverage = new Texture2D(1, 1, TextureFormat.RGBAFloat, false, true); owned.Add(coverage);
            coverage.SetPixel(0, 0, new Color(1, .5f, 0, 0)); coverage.Apply();
            var depth = new Texture2D(1, 1, TextureFormat.RGBAFloat, false, true); owned.Add(depth);
            depth.SetPixel(0, 0, new Color(.5f, 0, 0, 0)); depth.Apply();
            var source = new RenderTexture(width, width, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            var output = new RenderTexture(width, width, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            owned.Add(source); owned.Add(output); source.Create(); output.Create();
            var readback = new Texture2D(width, width, TextureFormat.RGBAFloat, false, true); owned.Add(readback);
            material.SetTexture("_LobbyColor", source); material.SetTexture("_LobbyCoverage", coverage);
            material.SetTexture("_CameraDepthTexture", depth); material.SetTexture("_LobbyForeground", Texture2D.whiteTexture);
            // Full shader viewport intentionally prevents shader discard from
            // masking a broken hardware scissor. Test an asymmetric region.
            material.SetVector("_LobbyViewport", new Vector4(0, 0, 1, 1));
            var region = new Rect(.125f, .625f, .375f, .25f);
            var background = new Color(.1f, .2f, .3f, .6f);
            var model = new Color(.7f, .4f, .2f, 1);
            for (int step = 0; step < 2; ++step)
            {
                command.Clear();
                command.SetGlobalVector("unity_OrthoParams", new Vector4(0, 0, 0, 1));
                command.SetGlobalVector("_ProjectionParams", new Vector4(1, .1f, 1, 1));
                command.SetRenderTarget(source); command.ClearRenderTarget(false, true, model);
                command.SetRenderTarget(output); command.ClearRenderTarget(false, true, background);
                if (step == 0) Entry.DrawComposite(command, source, output, material, region, width, width);
                else command.Blit(source, output, material, 1); // Scissor must have been restored.
                Graphics.ExecuteCommandBuffer(command);
                RenderTexture.active = output; readback.ReadPixels(new Rect(0, 0, width, width), 0, 0); readback.Apply();
                int changed = 0;
                for (int y = 0; y < width; ++y) for (int x = 0; x < width; ++x)
                {
                    bool inside = step == 1 || (x >= 4 && x < 16 && y >= 20 && y < 28);
                    var color = readback.GetPixel(x, y); var expected = inside ? model : background;
                    Require(Mathf.Abs(color.r - expected.r) < .002f && Mathf.Abs(color.g - expected.g) < .002f &&
                        Mathf.Abs(color.b - expected.b) < .002f && Mathf.Abs(color.a - background.a) < .002f,
                        "Composite hardware scissor/restore mismatch at " + x + "," + y + " step=" + step + " color=" + color);
                    if (Mathf.Abs(color.r - background.r) > .01f) ++changed;
                }
                Require(changed == (step == 0 ? 96 : 1024), "Unexpected composite raster area");
            }
            report.Add("Hardware composite scissor: 96/1024 pixels, asymmetric Y, destination alpha and subsequent full draw PASS");
        }
        finally
        {
            command.Release(); Shader.SetGlobalVector("unity_OrthoParams", oldOrtho);
            Shader.SetGlobalVector("_ProjectionParams", oldProjection); RenderTexture.active = previous;
            foreach (var item in owned) if (item) Object.DestroyImmediate(item);
        }
    }

    static IEnumerator ColorCopyRequirements(bool cameraRequest, bool featureRequest)
    {
        using (var f = new Fixture(false, false, false, false))
        {
            f.RendererData.intermediateTextureMode = IntermediateTextureMode.Auto;
            var probe = ScriptableObject.CreateInstance<LobbyCopyProbe>(); f.Owned.Add(probe);
            probe.RequestColor = featureRequest; f.RendererData.rendererFeatures.Add(probe);
            f.Camera.GetUniversalAdditionalCameraData().requiresColorTexture = cameraRequest;
            yield return Frames();
            var lobby = f.CaptureLobby(); f.EnterMatch();
            var data = f.Camera.GetUniversalAdditionalCameraData();
            data.requiresColorTexture = false; data.renderPostProcessing = false; data.requiresDepthTexture = false;
            LobbyCopyProbe.ResetCounts(); var result = f.Render();
            Require(Entry.GetStatus().Contains("active on"), "Auto intermediate target did not compose");
            Require(Difference(lobby, result, 45, 80, 14, 25) < .003f, "Auto target changed model appearance");
            Require(LobbyCopyProbe.PrivateCalls > 0 && LobbyCopyProbe.WorldCalls > 0, "CopyColor probe did not execute");
            Require(LobbyCopyProbe.PrivateCopies == ((cameraRequest || featureRequest) ? LobbyCopyProbe.PrivateCalls : 0),
                "Private camera opaque copy requirement changed");
            Require(LobbyCopyProbe.WorldCopies == (featureRequest ? LobbyCopyProbe.WorldCalls : 0),
                "Composite forced an unused world opaque copy");
            report.Add("Actual URP CopyColor passes: cameraRequest=" + cameraRequest + " featureRequest=" + featureRequest +
                " private=" + LobbyCopyProbe.PrivateCopies + " world=" + LobbyCopyProbe.WorldCopies + " Auto target PASS");
        }
    }

    static IEnumerator CroppedForeground(bool orthographic, bool clothing, bool multiple, bool msaa)
    {
        using (var f = new Fixture(true, true, false, true))
        {
            const int width = 1024, height = 768;
            var output = new RenderTexture(width, height, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            output.antiAliasing = msaa ? 4 : 1; output.Create(); f.Owned.Add(output);
            f.Camera.targetTexture = output; f.UI.targetTexture = output;
            f.Pipeline.msaaSampleCount = msaa ? 4 : 1; f.Camera.allowMSAA = msaa; f.UI.allowMSAA = msaa;
            f.Camera.orthographic = f.UI.orthographic = orthographic;
            f.Camera.orthographicSize = f.UI.orthographicSize = 1.7f;
            f.Self.transform.position = new Vector3(-.7f, .35f, 0);
            f.Other.transform.position = new Vector3(.7f, -.35f, 0);
            var hosts = new[] { f.Self.transform, multiple ? f.Other.transform : null, null };
            var renderers = new List<Renderer> { f.Self }; var owners = new List<int> { 0 };
            if (multiple) { renderers.Add(f.Other); owners.Add(1); }
            var transparent = new Material(Shader.Find("Universal Render Pipeline/Unlit")); f.Owned.Add(transparent);
            transparent.SetColor("_BaseColor", new Color(.1f, .7f, .25f, .5f));
            transparent.SetFloat("_Surface", 1); transparent.SetFloat("_ZWrite", 0);
            transparent.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha); transparent.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha);
            transparent.EnableKeyword("_SURFACE_TYPE_TRANSPARENT"); transparent.renderQueue = 3000;
            Renderer shell = null;
            if (clothing)
            {
                shell = GameObject.CreatePrimitive(PrimitiveType.Quad).GetComponent<Renderer>();
                shell.transform.SetParent(f.Self.transform, false); shell.transform.localPosition = new Vector3(0, 0, -.55f);
                shell.transform.localScale = new Vector3(.7f, .7f, 1); shell.gameObject.layer = 8;
                shell.sharedMaterial = transparent; renderers.Add(shell); owners.Add(0);
            }
            yield return Frames();
            f.Render(); Entry.SetTargets(f.Self.transform, f.Camera, hosts, renderers.ToArray(), owners.ToArray(), true);
            var lobby = f.Render(); f.EnterMatch(false);
            if (msaa)
            {
                f.Pipeline.msaaSampleCount = 1; f.Camera.allowMSAA = f.UI.allowMSAA = false;
                output.Release(); output.antiAliasing = 1; output.Create();
            }
            Entry.SetTargets(null, null, hosts, renderers.ToArray(), owners.ToArray(), true);
            var clear = f.Render();
            var samplePoints = new List<Vector2Int>();
            foreach (var host in hosts) if (host)
            {
                var point = f.Camera.WorldToViewportPoint(host.position);
                samplePoints.Add(new Vector2Int(Mathf.RoundToInt(point.x * width), Mathf.RoundToInt(point.y * height)));
            }
            foreach (var point in samplePoints)
                Require(PixelDifference(lobby, clear, width, point) < .003f, "Cropped clothing/model appearance changed");
            var region = Entry.ForegroundRegion();
            Require(region.width * region.height < .8f, "Foreground fixture did not crop");
            var barrier = GameObject.CreatePrimitive(PrimitiveType.Quad).GetComponent<Renderer>();
            barrier.gameObject.layer = 10; barrier.sharedMaterial = transparent;
            barrier.transform.position = new Vector3(0, 0, -.8f); barrier.transform.localScale = new Vector3(12, 12, 1);
            var blocked = f.Render();
            var mask = Entry.GetModelTarget(0, "foreground");
            var readback = new Texture2D(1, 1, TextureFormat.RGBAFloat, false, true); f.Owned.Add(readback);
            var previous = RenderTexture.active; RenderTexture.active = mask;
            try
            {
                foreach (var point in samplePoints)
                {
                    readback.ReadPixels(new Rect(point.x, point.y, 1, 1), 0, 0); readback.Apply();
                    Require(readback.GetPixel(0, 0).r < 10, "Cropped foreground omitted a visible barrier");
                }
                readback.ReadPixels(new Rect(0, 0, 1, 1), 0, 0); readback.Apply();
                Require(readback.GetPixel(0, 0).r > 1e10f, "Foreground drew outside the union scissor");
            }
            finally { RenderTexture.active = previous; }
            if (shell) Require(!shell.forceRenderingOff, "Foreground cull failed to restore clothing");
            Entry.SetTarget(null, null, true); var original = f.Render();
            foreach (var point in samplePoints)
                Require(PixelDifference(blocked, original, width, point) < .003f, "Cropped foreground was overwritten");
            // Re-enable composition after the mask changed: no stale foreground.
            barrier.enabled = false;
            Entry.SetTargets(null, null, hosts, renderers.ToArray(), owners.ToArray(), true);
            var restored = f.Render();
            foreach (var point in samplePoints)
                Require(PixelDifference(lobby, restored, width, point) < .003f, "Moving foreground left a stale mask");
            report.Add("Cropped foreground 1024x768 ortho=" + orthographic + " clothing=" + clothing + " multiple=" + multiple +
                " MSAA=" + msaa + " union=" + (region.width * region.height * 100).ToString("F1") +
                "% barrier/clear/movement/restore PASS");
        }
    }

    static float PixelDifference(Color[] a, Color[] b, int width, Vector2Int center)
    {
        float error = 0;
        for (int y = center.y - 5; y < center.y + 5; ++y) for (int x = center.x - 5; x < center.x + 5; ++x)
        {
            var delta = a[y * width + x] - b[y * width + x];
            error = Mathf.Max(error, Mathf.Abs(delta.r), Mathf.Abs(delta.g), Mathf.Abs(delta.b));
        }
        return error;
    }

    static IEnumerator Suite()
    {
        yield return PropertyProbe();
        CompositeScissor();
        yield return ColorCopyRequirements(false, false);
        yield return ColorCopyRequirements(true, false);
        yield return ColorCopyRequirements(false, true);
        yield return CroppedForeground(false, false, false, false);
        yield return CroppedForeground(true, true, true, true);
        RotateProbe();
        CropHysteresis();
        RenderBufferReuse();
        OriginalRestoration();
        GeometryCache();
        ShaderProbe.VerifyReflection(AssetDatabase.LoadAssetAtPath<Shader>("Assets/LobbyModel/LobbyModel.shader"));
        yield return Appearance(false, false, false, false);
        yield return Appearance(true, true, false, false);
        yield return AllModels();
        yield return RabbitInitialCapture();
        yield return Appearance(true, false, true, true);
        yield return Appearance(true, true, false, true);
        yield return ProfileOnUICamera();
        yield return Multisampling();
        yield return RendererFeatures();
        yield return LayerFeaturesAndDynamicObjects();
        yield return CroppedModel(false);
        yield return CroppedModel(true);
        yield return CropRecovery();
        yield return EmptyReflectionProbe();
        if (RealLobbyModel.Enabled)
        {
            yield return OpaqueMaterialAlpha(false);
            yield return OpaqueMaterialAlpha(true);
            yield return OpaqueMaterialAlpha(false, true);
            yield return OpaqueMaterialAlpha(false, false, true);
        }
        yield return PerPartLighting();
        yield return ReflectionAcrossPositions();
        yield return SkinnedMotion();
        yield return SharedModelInstances();
        yield return ExplicitSourceAndStatus();
        yield return OwnTransparentForeground();
        if (RealLobbyModel.Enabled)
            foreach (var shader in new[] { "FTREZ/SakiSource", "FTREZ/EeveeToon", "FTREZ/GLTF" })
            {
                yield return RealModel(shader, false);
                yield return RealModel(shader, true);
            }
        else report.Add("Real GLB validation: SKIPPED (supply --validation-model)");
        yield return Foreground(false);
        yield return Foreground(true);
        yield return Foreground(false, true);
        yield return Foreground(true, true);
        yield return FailureAndVisibility();
        Require(errors.Count == 0, "Unexpected Unity errors: " + string.Join(" | ", errors));
    }

    static void Step()
    {
        try
        {
            while (steps.Count != 0)
            {
                var current = steps.Peek();
                if (!current.MoveNext()) { (steps.Pop() as IDisposable)?.Dispose(); continue; }
                if (current.Current is IEnumerator child) { steps.Push(child); continue; }
                EditorApplication.QueuePlayerLoopUpdate();
                return;
            }
            Finish(null);
        }
        catch (Exception error) { Finish(error); }
    }

    static void Finish(Exception error)
    {
        EditorApplication.update -= Step;
        while (steps.Count != 0) (steps.Pop() as IDisposable)?.Dispose();
        report.Add(error == null ? "PASS" : "FAIL: " + error);
        Application.logMessageReceived -= Log;
        File.WriteAllLines("Validation/result.txt", report);
        Entry.Shutdown();
        EditorApplication.Exit(error == null ? 0 : 1);
    }
    static void Log(string condition, string trace, LogType type)
    { if (type == LogType.Error || type == LogType.Exception || type == LogType.Assert) errors.Add(condition); }
}
