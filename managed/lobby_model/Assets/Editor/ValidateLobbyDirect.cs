using System.Collections;
using System;
using FTRez.LobbyModel;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using Object = UnityEngine.Object;

public static partial class ValidateLobbyModel
{
    static bool propertyProbe;
    public static void Probe() { propertyProbe = true; Run(); }
    static IEnumerator PropertyProbe()
    {
        yield return DirectAppearance(false);
        yield return DirectAppearance(true);
        yield return DirectAngles();
        yield return DirectReflection();
        yield return DirectForeground(false);
        yield return DirectForeground(true);
        yield return DirectWorldLights();
        if (RealLobbyModel.Enabled)
            foreach (var shader in new[] { "FTREZ/SakiSource", "FTREZ/EeveeToon", "FTREZ/GLTF" })
                yield return DirectRealModel(shader);
    }

    static void DirectWorld(Fixture f, bool world)
    {
        f.Light.color = world ? new Color(.05f, .15f, 1) : new Color(1, .85f, .65f);
        f.Light.intensity = world ? 2 : 1.4f;
        RenderSettings.ambientLight = world ? new Color(.01f, .02f, .3f) : new Color(.15f, .1f, .08f);
    }

    static void NoComposition()
    {
        Require(!Entry.GetModelTarget(0, "colorA") && !Entry.GetModelTarget(0, "coverage"), "Direct path allocated composite textures");
        Require((Camera)typeof(Entry).GetField("privateCamera", flags).GetValue(null) == null, "Direct path created a private camera");
        Require((int)typeof(Entry).GetField("privateCalls", flags).GetValue(null) == 0, "Direct path rendered an extra camera");
    }

    static IEnumerator DirectAppearance(bool transparent)
    {
        using (var f = new Fixture(false, false, false, true, true))
        {
            if (transparent)
            {
                var material = new Material(f.Self.sharedMaterial); f.Owned.Add(material); f.Self.sharedMaterial = material;
                material.SetFloat("_Surface", 1); material.SetFloat("_ZWrite", 0);
                material.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha); material.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha);
                material.SetColor("_BaseColor", new Color(.45f, .28f, .12f, .35f));
                material.EnableKeyword("_SURFACE_TYPE_TRANSPARENT"); material.renderQueue = 3000;
                material.SetShaderPassEnabled("DepthOnly", false);
            }
            // Indexed properties must be respected and restored, including alpha.
            var indexed = new MaterialPropertyBlock(); indexed.SetFloat("_Smoothness", .07f);
            indexed.SetFloat("_FTRezIndexedMarker", .73f); f.Self.SetPropertyBlock(indexed, 0);
            yield return Frames();
            var lobby = f.CaptureLobby();
            DirectWorld(f, true); Entry.SetTarget(f.Self.transform, new[] { f.Self }, true);
            var result = f.Render();
            float error = Difference(result, lobby, 0, 0, size / 2, size);
            Require(error < .005f, "Direct appearance/edge/alpha mismatch " + transparent + ": " + error);
            NoComposition();
            Require(Entry.GetStatus().Contains("active on"), "Direct camera did not apply lighting");
            typeof(Entry).GetField("lastCompositeTime", flags).SetValue(null, Time.realtimeSinceStartup - 5);
            Require(!Entry.GetStatus().Contains("active on"), "Direct status reported stale camera activity");
            f.Render(); Require(Entry.GetStatus().Contains("active on"), "Direct camera activity did not resume");
            f.Self.GetPropertyBlock(indexed, 0);
            Require(Mathf.Abs(indexed.GetFloat("_FTRezIndexedMarker") - .73f) < 1e-6f &&
                !indexed.HasVector("_MainLightColor"), "Direct indexed MPB leaked");
            Require(f.Self.lightProbeUsage == LightProbeUsage.BlendProbes && f.Light.enabled, "Direct renderer state leaked");
            Entry.SetTarget(null, null, true); var world = f.Render();
            Require(Difference(world, lobby, 46, 78, 15, 6) > .02f, "Direct fixture does not exercise lighting");
            Require(Difference(result, world, size / 2, 0, size / 2, size) < .002f, "Direct lighting changed another model");
            // Shared assets, independent targets; include the own Profile slot.
            Entry.SetTargets(null, null, new[] { f.Self.transform, null, f.Other.transform },
                new[] { f.Self, f.Other }, new[] { 0, 2 }, true);
            var both = f.Render();
            Require(Difference(both, lobby, 129, 78, 15, 25) < .005f, "Direct Profile slot lost lighting");
            // No main light exists in this camera's cull result.
            f.Light.enabled = false;
            var noSun = f.Render();
            Require(Difference(noSun, both, 0, 0, size, size) < .005f, "World main-light culling disabled the captured main light");
            Entry.SetTargets(null, null, new Transform[3], null, null, false);
            var disabled = f.Render();
            Require(Difference(disabled, noSun, 46, 78, 15, 6) > .02f, "Disable did not restore world lighting");
            report.Add("Direct ordinary draw: transparent=" + transparent + " full silhouette error=" + error +
                "; indexed MPB, UI, independent Profile, no-world-sun, disable PASS");
        }
    }

    static IEnumerator DirectAngles()
    {
        using (var f = new Fixture(false, false, false, false, true))
        {
            f.Other.enabled = false;
            var source = f.Self; source.enabled = false;
            var mesh = Object.Instantiate(source.GetComponent<MeshFilter>().sharedMesh); f.Owned.Add(mesh);
            var root = new GameObject("Direct animated model").transform; root.SetParent(f.Camera.transform.parent, false);
            root.position = source.transform.position;
            var bone = new GameObject("Head").transform; bone.SetParent(root, false);
            var go = new GameObject("Direct head skin"); go.layer = 8; go.transform.SetParent(root, false);
            var skin = go.AddComponent<SkinnedMeshRenderer>(); skin.sharedMesh = mesh; skin.sharedMaterial = source.sharedMaterial;
            var weights = new BoneWeight[mesh.vertexCount];
            for (int i = 0; i < weights.Length; ++i) weights[i] = new BoneWeight { boneIndex0 = 0, weight0 = 1 };
            mesh.boneWeights = weights; mesh.bindposes = new[] { Matrix4x4.identity };
            skin.bones = new[] { bone }; skin.rootBone = bone; skin.localBounds = new Bounds(Vector3.zero, Vector3.one * 6);
            skin.updateWhenOffscreen = true; f.Self = skin;
            yield return Frames(); f.CaptureLobby();
            float maxError = 0;
            foreach (float pitch in new[] { -80f, -45f, 0f, 30f, 60f, 85f })
            {
                bone.localRotation = Quaternion.Euler(pitch, 12, 0);
                bone.localScale = new Vector3(.7f, 1.5f, .8f);
                bone.localPosition = new Vector3(0, .15f + pitch * .001f, 0);
                f.Camera.transform.LookAt(root.position + new Vector3(0, pitch * .01f, 0));
                yield return Frames();
                Entry.SetTarget(null, null, true); DirectWorld(f, false); var reference = f.Render();
                Entry.SetTarget(skin.transform, new Renderer[] { skin }, true); DirectWorld(f, true);
                var result = f.Render();
                maxError = Mathf.Max(maxError, Difference(result, reference, 0, 0, size, size));
            }
            Require(maxError < .006f, "Head/camera pitch exposed world-lit pixels: " + maxError);
            // Crossing the camera near plane must keep using ordinary clipping.
            f.Camera.transform.SetPositionAndRotation(root.position + new Vector3(0, 0, -.35f), Quaternion.identity);
            Entry.SetTarget(null, null, true); DirectWorld(f, false); var nearReference = f.Render();
            Entry.SetTarget(skin.transform, new Renderer[] { skin }, true); DirectWorld(f, true); var near = f.Render();
            Require(Difference(near, nearReference, 0, 0, size, size) < .006f, "Near-plane crossing lost direct lighting");
            NoComposition();
            report.Add("Direct live skinned head/camera pitch -80..85 degrees, silhouette and near-plane clipping: max=" + maxError + " PASS");
        }
    }

    static IEnumerator DirectReflection()
    {
        using (var f = new Fixture(true, false, false, false, true))
        {
            f.Self.sharedMaterial.SetFloat("_Metallic", 1); f.Self.sharedMaterial.SetFloat("_Smoothness", .8f);
            var warm = SolidCube(new Color(.6f, .2f, .03f, 1), f.Owned);
            var cold = SolidCube(new Color(.02f, .3f, 1, 1), f.Owned);
            RenderSettings.defaultReflectionMode = DefaultReflectionMode.Custom;
            RenderSettings.customReflectionTexture = warm; RenderSettings.reflectionIntensity = .65f;
            var probe = new GameObject("Direct lobby reflection").AddComponent<ReflectionProbe>();
            probe.transform.position = f.Self.transform.position; probe.size = Vector3.one * 4;
            probe.mode = ReflectionProbeMode.Custom; probe.customBakedTexture = warm; probe.intensity = 1.7f;
            yield return Frames(); var lobby = f.CaptureLobby();
            DirectWorld(f, true); probe.customBakedTexture = cold; RenderSettings.customReflectionTexture = cold;
            Entry.SetTarget(f.Self.transform, new[] { f.Self }, true); yield return Frames(); var result = f.Render();
            float error = Difference(result, lobby, 46, 78, 15, 6);
            Require(error < .006f, "Direct captured reflection mismatch: " + error);
            var move = new Vector3(12, 3, 0); f.Self.transform.position += move; f.Camera.transform.position += move;
            probe.transform.position += move; probe.boxProjection = true; probe.importance = 30;
            yield return Frames(); var moved = f.Render();
            Require(Difference(moved, result, 46, 78, 15, 6) < .006f, "World reflection probe switched direct model appearance");
            Require(probe.enabled && RenderSettings.customReflectionTexture == cold, "Direct reflection mutated world state");
            NoComposition();
            report.Add("Direct metallic reflection, intensity, world probe replacement/movement: error=" + error + " PASS");
        }
    }

    static IEnumerator DirectRealModel(string shader)
    {
        using (var f = new Fixture(false, false, false, false, true))
        {
            f.Self.enabled = f.Other.enabled = false;
            var root = new GameObject("Real direct " + shader).transform; root.SetParent(f.Camera.transform.parent, false);
            var own = RealLobbyModel.Create(RealLobbyModel.Load(), root, shader, f.Owned);
            yield return Frames(); f.Render();
            Entry.SetTargets(root, f.Camera, new[] { root, null, null }, own, new int[own.Length], true);
            var lobby = f.Render();
            DirectWorld(f, true); Entry.SetTargets(null, null, new[] { root, null, null }, own, new int[own.Length], true);
            var result = f.Render();
            float error = Difference(result, lobby, 0, 0, size, size);
            Require(error < .01f, "Real direct shader mismatch " + shader + ": " + error);
            NoComposition();
            report.Add("Direct real Saki GLB " + shader + " full-frame max=" + error + " PASS");
        }
    }

    static IEnumerator DirectForeground(bool transparent)
    {
        using (var f = new Fixture(true, false, false, true, true))
        {
            f.Pipeline.msaaSampleCount = 4; f.Camera.allowMSAA = true;
            var target = f.Camera.targetTexture; target.Release(); target.antiAliasing = 4; target.Create();
            yield return Frames();
            f.CaptureLobby();
            var quad = GameObject.CreatePrimitive(PrimitiveType.Quad); quad.layer = 10;
            quad.transform.position = new Vector3(-.7f, 0, -.7f); quad.transform.localScale = new Vector3(.6f, .6f, 1);
            var material = new Material(Shader.Find("Universal Render Pipeline/Unlit")); f.Owned.Add(material);
            material.SetColor("_BaseColor", new Color(.1f, .7f, .25f, transparent ? .5f : 1));
            if (transparent)
            {
                material.SetFloat("_Surface", 1); material.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha);
                material.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha); material.SetFloat("_ZWrite", 0);
                material.EnableKeyword("_SURFACE_TYPE_TRANSPARENT"); material.renderQueue = 3000;
            }
            quad.GetComponent<Renderer>().sharedMaterial = material;
            float error = 0;
            foreach (float z in new[] { -.7f, .7f, -.52f })
            {
                quad.transform.position = new Vector3(-.7f, 0, z);
                Entry.SetTarget(null, null, true); DirectWorld(f, false); var reference = f.Render();
                Entry.SetTarget(f.Self.transform, new[] { f.Self }, true); DirectWorld(f, true); var result = f.Render();
                error = Mathf.Max(error, Difference(result, reference, 0, 0, size / 2, size));
            }
            Require(error < .006f, "Direct foreground sorting/occlusion mismatch: " + error);
            NoComposition();
            report.Add("Direct foreground front/behind/intersection transparent=" + transparent + " MSAA=4 UI=true: error=" + error + " PASS");
        }
    }

    static IEnumerator DirectWorldLights()
    {
        using (var f = new Fixture(false, false, false, false, true))
        {
            typeof(UniversalRenderPipelineAsset).GetField("m_SupportsLightLayers", flags).SetValue(f.Pipeline, true);
            yield return Frames(); var lobby = f.CaptureLobby();
            var point = new GameObject("New world point light").AddComponent<Light>();
            point.type = LightType.Point; point.color = Color.green; point.intensity = 12; point.range = 8;
            point.transform.position = new Vector3(0, 0, -1);
            f.Light.shadows = LightShadows.Hard;
            f.Light.GetUniversalAdditionalLightData().renderingLayers = 2;
            RenderSettings.fog = true; RenderSettings.fogMode = FogMode.ExponentialSquared; RenderSettings.fogDensity = .25f;
            RenderSettings.fogColor = Color.magenta;
            DirectWorld(f, true); Entry.SetTarget(f.Self.transform, new[] { f.Self }, true);
            var result = f.Render();
            float error = Difference(result, lobby, 46, 78, 15, 6);
            Require(error < .006f, "World additional lights/fog changed captured model lighting: " + error);
            Entry.SetTarget(null, null, true); var world = f.Render();
            Require(Difference(result, world, 129, 78, 15, 25) < .002f, "Light/fog override affected world");
            Require(Difference(world, lobby, 46, 78, 15, 6) > .05f, "World light fixture lacks visible change");
            report.Add("Direct world point-light/shadow variant/light layers/fog isolation: error=" + error + " PASS");
        }
    }
}
