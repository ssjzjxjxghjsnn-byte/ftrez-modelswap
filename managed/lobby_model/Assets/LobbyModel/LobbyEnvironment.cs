using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.SceneManagement;
using Object = UnityEngine.Object;

namespace FTRez.LobbyModel
{
    // A snapshot of the environment that actually lit the lobby character.
    // Nothing is applied while a normal game camera is being rendered.
    internal sealed class LobbyEnvironment : IDisposable
    {
        sealed class LightCopy
        {
            public Light Light;
            public Vector3 Position;
            public Quaternion Rotation;
            public int Mask;
        }
        sealed class ProbeCopy
        {
            public ReflectionProbe Probe;
            public Vector3 Position;
            public Quaternion Rotation;
            public int Mask;
        }
        readonly List<LightCopy> lights = new List<LightCopy>();
        readonly List<ProbeCopy> probes = new List<ProbeCopy>();
        readonly List<SphericalHarmonicsL2> harmonics = new List<SphericalHarmonicsL2>();
        readonly List<Vector3> probePositions = new List<Vector3>();
        readonly List<Vector4> probeOcclusion = new List<Vector4>();
        readonly List<int> layers = new List<int>();
        readonly List<Renderer> sampledRenderers = new List<Renderer>();
        readonly List<Mesh> sampledMeshes = new List<Mesh>();
        public int ModelMask { get; private set; }
        readonly MaterialPropertyBlock lightingBlock = new MaterialPropertyBlock();
        readonly SphericalHarmonicsL2[] shBuffer = new SphericalHarmonicsL2[1];
        readonly Vector4[] occlusionBuffer = new Vector4[1];
        readonly Dictionary<Transform, RotatedLighting> rotatedLighting = new Dictionary<Transform, RotatedLighting>();
        readonly List<Light> suppressedLights = new List<Light>();
        readonly List<ReflectionProbe> suppressedProbes = new List<ReflectionProbe>();
        readonly List<ReflectionProbeBlendInfo> closestProbes = new List<ReflectionProbeBlendInfo>();
        static readonly LightType[] lightTypes = { LightType.Directional, LightType.Point, LightType.Spot };
        sealed class RotatedLighting
        {
            public Quaternion Rotation;
            public readonly Dictionary<int, SphericalHarmonicsL2> Samples = new Dictionary<int, SphericalHarmonicsL2>();
        }

        SphericalHarmonicsL2 RotatedSample(Transform host, int sample, Quaternion delta)
        {
            if (!rotatedLighting.TryGetValue(host, out var cache))
            {
                // At most three live targets; do not retain every swapped host.
                if (rotatedLighting.Count >= 3) rotatedLighting.Clear();
                cache = new RotatedLighting { Rotation = delta }; rotatedLighting.Add(host, cache);
            }
            if (!cache.Rotation.Equals(delta)) { cache.Rotation = delta; cache.Samples.Clear(); }
            if (!cache.Samples.TryGetValue(sample, out var sh))
            {
                sh = Harmonics.Rotate(sample >= 0 ? harmonics[sample] : settings.AmbientProbe, delta);
                cache.Samples.Add(sample, sh);
            }
            return sh;
        }
        sealed class FrozenTexture { public Texture Source, Copy; public Vector4 Decode; }
        readonly List<FrozenTexture> frozenTextures = new List<FrozenTexture>();
        Settings settings;
        Vector3 origin;
        Quaternion rotation;
        Light sun;
        bool disposed;
        bool loggedProbeSelection;
        public bool Ready { get; private set; }
        public Scene Scene { get; private set; }

        public void Capture(Camera camera, Transform host, Renderer[] own)
        {
            if (Ready || disposed) throw new InvalidOperationException("Environment snapshot already used");
            try
            {
                settings = new Settings();
                settings.FreezeReflection(FreezeTexture);
                Scene = SceneManager.CreateScene("FTREZ.Lobby.Isolated." + Guid.NewGuid());
                origin = host.position; rotation = host.rotation;
                int ownMask = 0;
                var closest = new List<ReflectionProbeBlendInfo>();
                var captured = new HashSet<ReflectionProbe>();
                foreach (var renderer in own)
                {
                    SphericalHarmonicsL2 sh = settings.AmbientProbe;
                    Vector3 samplePosition = host.position;
                    Vector4 occlusion = Vector4.one;
                    if (renderer)
                    {
                        ownMask |= 1 << renderer.gameObject.layer;
                        samplePosition = renderer.probeAnchor ? renderer.probeAnchor.position : renderer.bounds.center;
                        var shValues = new SphericalHarmonicsL2[1]; var occlusionValues = new Vector4[1];
                        LightProbes.CalculateInterpolatedLightAndOcclusionProbes(new[] { samplePosition }, shValues, occlusionValues);
                        sh = shValues[0]; occlusion = occlusionValues[0];
                        LogPart(renderer);
                        closest.Clear(); renderer.GetClosestReflectionProbes(closest);
                        foreach (var item in closest)
                            if (item.probe && item.weight > 0 && captured.Add(item.probe)) CaptureProbe(item.probe);
                    }
                    harmonics.Add(sh);
                    layers.Add(renderer ? renderer.gameObject.layer : host.gameObject.layer);
                    sampledRenderers.Add(renderer); sampledMeshes.Add(MeshOf(renderer));
                    ModelMask |= 1 << layers[layers.Count - 1];
                    probePositions.Add(host.InverseTransformPoint(samplePosition)); probeOcclusion.Add(occlusion);
                }
                foreach (var source in Object.FindObjectsOfType<Light>())
                {
                    if (!source || !source.isActiveAndEnabled || (source.cullingMask & ownMask & camera.cullingMask) == 0)
                        continue;
                    var go = NewObject("FTREZ.Lobby.Light");
                    var light = go.AddComponent<Light>();
                    light.enabled = false;
                    lights.Add(new LightCopy { Light = light, Position = source.transform.position, Rotation = source.transform.rotation, Mask = source.cullingMask });
                    light.type = source.type; light.color = source.color; light.intensity = source.intensity;
                    light.cullingMask = source.cullingMask;
                    light.range = source.range; light.spotAngle = source.spotAngle; light.innerSpotAngle = source.innerSpotAngle;
                    light.bounceIntensity = source.bounceIntensity; light.colorTemperature = source.colorTemperature;
                    light.useColorTemperature = source.useColorTemperature; light.cookie = source.cookie;
                    light.cookieSize = source.cookieSize; light.renderMode = source.renderMode;
                    light.shadows = source.shadows; light.shadowStrength = source.shadowStrength;
                    light.shadowResolution = source.shadowResolution; light.shadowCustomResolution = source.shadowCustomResolution;
                    light.shadowBias = source.shadowBias; light.shadowNormalBias = source.shadowNormalBias;
                    light.shadowNearPlane = source.shadowNearPlane;
                    var extra = source.GetComponent<UniversalAdditionalLightData>();
                    if (extra) JsonUtility.FromJsonOverwrite(JsonUtility.ToJson(extra), go.AddComponent<UniversalAdditionalLightData>());
                    // These copies must be evaluated as realtime lights in the private scene.
                    light.bakingOutput = source.bakingOutput;
                    if (source == settings.Sun) sun = light;
                }
                var previousScene = SceneManager.GetActiveScene();
                int previousLOD = Shader.globalMaximumLOD;
                try
                {
                    if (!SceneManager.SetActiveScene(Scene)) throw new InvalidOperationException("Cannot select isolated lobby scene");
                    settings.Apply(); settings.ApplyCapturedReflection(); RenderSettings.sun = sun;
                }
                finally { SceneManager.SetActiveScene(previousScene); Shader.globalMaximumLOD = previousLOD; }
                Ready = true;
            }
            catch { Dispose(); throw; }
        }

        GameObject NewObject(string name)
        {
            var go = new GameObject(name) { hideFlags = HideFlags.HideAndDontSave };
            SceneManager.MoveGameObjectToScene(go, Scene);
            return go;
        }

        internal static string DescribeTexture(Texture texture)
        {
            return texture ? texture.name + "#" + texture.GetInstanceID() + " " + texture.graphicsFormat +
                " " + texture.width + "x" + texture.height + " mips=" + texture.mipmapCount : "<null>";
        }

        static void LogPart(Renderer renderer)
        {
            Debug.Log("[FTREZ-LOBBY] part=" + renderer.name + " id=" + renderer.GetInstanceID() +
                " layer=" + renderer.gameObject.layer + " lightProbes=" + renderer.lightProbeUsage +
                " reflection=" + renderer.reflectionProbeUsage + " mpb=" + renderer.HasPropertyBlock());
            var materials = new List<Material>(); renderer.GetSharedMaterials(materials);
            for (int i = 0; i < materials.Count; ++i)
            {
                var material = materials[i]; if (!material) continue;
                Debug.Log("[FTREZ-LOBBY] part=" + renderer.name + " slot=" + i + " shader=" + material.shader.name +
                    " queue=" + material.renderQueue + " baseAlpha=" +
                    (material.HasProperty("_BaseColor") ? material.GetColor("_BaseColor").a.ToString() : "n/a"));
            }
        }

        void CaptureProbe(ReflectionProbe source)
        {
            var go = NewObject("FTREZ.Lobby.Reflection");
            var probe = go.AddComponent<ReflectionProbe>();
            probe.enabled = false;
            probes.Add(new ProbeCopy { Probe = probe, Position = source.transform.position, Rotation = source.transform.rotation, Mask = source.cullingMask });
            probe.mode = ReflectionProbeMode.Custom; probe.customBakedTexture = FreezeTexture(source.texture, source.textureHDRDecodeValues);
            probe.refreshMode = ReflectionProbeRefreshMode.ViaScripting;
            probe.boxProjection = source.boxProjection; probe.center = source.center; probe.size = source.size;
            probe.blendDistance = source.blendDistance; probe.importance = source.importance;
            // textureHDRDecodeValues already includes the source intensity.
            probe.intensity = 1; probe.cullingMask = source.cullingMask;
            Debug.Log("[FTREZ-LOBBY] reflection probe texture=" + DescribeTexture(source.texture) +
                " decode source=" + source.textureHDRDecodeValues + " snapshot=" + probe.textureHDRDecodeValues);
        }

        Texture FreezeTexture(Texture source, Vector4 decode)
        {
            if (!source) return null;
            foreach (var frozen in frozenTextures)
                if (frozen.Source == source && frozen.Decode.Equals(decode)) return frozen.Copy;
            Texture copy;
            var rendered = ReflectionSnapshot.Freeze(source, decode, Entry.SnapshotShader);
            try { copy = ReflectionSnapshot.ToCubemap(rendered); }
            finally { rendered.Release(); Entry.DestroyOwned(rendered); }
            frozenTextures.Add(new FrozenTexture { Source = source, Copy = copy, Decode = decode }); return copy;
        }

        static Mesh MeshOf(Renderer renderer)
        {
            if (!renderer) return null;
            if (renderer is SkinnedMeshRenderer skin) return skin.sharedMesh;
            var filter = renderer.GetComponent<MeshFilter>(); return filter ? filter.sharedMesh : null;
        }

        int SampleLayer(Renderer original, int nearest)
        {
            int identity = sampledRenderers.IndexOf(original);
            if (identity >= 0) return layers[identity];
            // Swaps can recreate renderers but keep the same mesh. Only use a
            // mesh match when every matching sample agrees on its lobby layer.
            var mesh = MeshOf(original); int matched = -1;
            if (mesh) for (int i = 0; i < sampledMeshes.Count; ++i) if (sampledMeshes[i] == mesh)
            {
                if (matched >= 0 && matched != layers[i]) return layers[Mathf.Max(0, nearest)];
                matched = layers[i];
            }
            return matched >= 0 ? matched : layers[Mathf.Max(0, nearest)];
        }

        public IDisposable Apply(Transform host, OriginalModel model, Renderer[] originals, LayerIsolation isolation)
        {
            if (!Ready || disposed) throw new InvalidOperationException("No lobby environment captured");
            return new Scope(this, host, model, originals, isolation);
        }

        sealed class Scope : IDisposable
        {
            readonly LobbyEnvironment snapshot;
            readonly Scene previousScene;
            readonly int previousLOD;
            readonly List<Light> suppressedLights;
            readonly List<ReflectionProbe> suppressedProbes;
            bool disposed;
            public Scope(LobbyEnvironment snapshot, Transform host, OriginalModel model, Renderer[] originals, LayerIsolation isolation)
            {
                this.snapshot = snapshot; previousScene = SceneManager.GetActiveScene(); previousLOD = Shader.globalMaximumLOD;
                var own = model.Renderers;
                suppressedLights = snapshot.suppressedLights; suppressedLights.Clear();
                suppressedProbes = snapshot.suppressedProbes; suppressedProbes.Clear();
                try
                {
                    // Ask the engine only for lights affecting the private draw
                    // layers, instead of enumerating every scene component.
                    for (int layer = 8; layer <= 30; ++layer) if ((isolation.Mask & (1 << layer)) != 0)
                        foreach (var type in lightTypes)
#pragma warning disable 618
                            foreach (var light in Light.GetLights(type, layer))
#pragma warning restore 618
                                if (light && light.enabled && !suppressedLights.Contains(light))
                                { suppressedLights.Add(light); light.enabled = false; }
                    // Only probes intersecting the character can contribute.
                    var closest = snapshot.closestProbes;
                    foreach (var renderer in own) if (renderer)
                    {
                        bool found;
                        do
                        {
                            found = false; closest.Clear(); renderer.GetClosestReflectionProbes(closest);
                            foreach (var candidate in closest)
                                if (candidate.probe && candidate.probe.enabled && !suppressedProbes.Contains(candidate.probe))
                                { suppressedProbes.Add(candidate.probe); candidate.probe.enabled = false; found = true; }
                        } while (found);
                    }
                    if (!SceneManager.SetActiveScene(snapshot.Scene)) throw new InvalidOperationException("Isolated lobby scene was unloaded");
                    snapshot.settings.ApplyLOD();
                    Quaternion delta = host.rotation * Quaternion.Inverse(snapshot.rotation);
                    foreach (var copy in snapshot.lights)
                    {
                        copy.Light.transform.SetPositionAndRotation(host.position + delta * (copy.Position - snapshot.origin), delta * copy.Rotation);
                        copy.Light.cullingMask = isolation.Map(copy.Mask);
                        copy.Light.enabled = true;
                    }
                    foreach (var copy in snapshot.probes)
                    {
                        copy.Probe.transform.SetPositionAndRotation(host.position + delta * (copy.Position - snapshot.origin), delta * copy.Rotation);
                        // Keep captured probes on the private camera's layers.
                        isolation.SetLayer(copy.Probe.gameObject, isolation.Layer(snapshot.layers[0]));
                        copy.Probe.cullingMask = isolation.Map(copy.Mask);
                        copy.Probe.enabled = true;
                    }
                    // Enable/move happens inside a camera callback, after the
                    // engine's usual frame update. Refresh before private culling.
                    if (snapshot.probes.Count != 0 || suppressedProbes.Count != 0)
                        ReflectionProbe.UpdateCachedState();
                    for (int i = 0; i < own.Length; ++i)
                    {
                        var renderer = own[i]; if (!renderer) continue;
                        if (!snapshot.loggedProbeSelection && snapshot.probes.Count > 0)
                        {
                            closest.Clear(); renderer.GetClosestReflectionProbes(closest);
                            Debug.Log("[FTREZ-LOBBY] private closest probes=" + closest.Count + " usage=" + renderer.reflectionProbeUsage +
                                " snapshot=" + snapshot.probes[0].Probe.texture + " bounds=" + snapshot.probes[0].Probe.bounds);
                            snapshot.loggedProbeSelection = true;
                        }
                        // Renderer ordering/count changes with model swaps and
                        // Rabbit/Profile. Select the lobby sample by location.
                        var position = host.InverseTransformPoint(renderer.probeAnchor ? renderer.probeAnchor.position : renderer.bounds.center);
                        // Keep a captured part's own sample while it animates.
                        // Nearest-centre reassignment can swap face/hair/clothing
                        // lighting when neighbouring parts move past each other.
                        int nearest = snapshot.sampledRenderers.IndexOf(originals[i]);
                        float distance = float.PositiveInfinity;
                        if (nearest < 0) for (int j = 0; j < snapshot.probePositions.Count; ++j)
                        {
                            float candidate = (position - snapshot.probePositions[j]).sqrMagnitude;
                            if (candidate < distance) { distance = candidate; nearest = j; }
                        }
                        isolation.SetLayer(renderer.gameObject, isolation.Layer(snapshot.SampleLayer(originals[i], nearest)));
                        // Authored MPBs already belong to this exact renderer.
                        if (model.SourceProbeUsage(i) == LightProbeUsage.CustomProvided) continue;
                        if (model.SourceProbeUsage(i) == LightProbeUsage.Off) nearest = -1;
                        model.PrepareLighting(i);
                        snapshot.ApplyLighting(renderer, snapshot.RotatedSample(host, nearest, delta),
                            nearest >= 0 ? snapshot.probeOcclusion[nearest] : Vector4.one, model.MaterialBlockSlots[i]);
                    }
                }
                catch { Dispose(); throw; }
            }
            public void Dispose()
            {
                if (disposed) return; disposed = true;
                foreach (var copy in snapshot.lights) if (copy.Light) copy.Light.enabled = false;
                foreach (var copy in snapshot.probes) if (copy.Probe) copy.Probe.enabled = false;
                SceneManager.SetActiveScene(previousScene);
                Shader.globalMaximumLOD = previousLOD;
                foreach (var probe in suppressedProbes) if (probe) probe.enabled = true;
                if (snapshot.probes.Count != 0 || suppressedProbes.Count != 0)
                    ReflectionProbe.UpdateCachedState();
                foreach (var light in suppressedLights) if (light) light.enabled = true;
            }
        }

        void ApplyLighting(Renderer renderer, SphericalHarmonicsL2 sh, Vector4 occlusion, List<int> materialBlocks)
        {
            // OriginalModel restores these overrides before the world camera.
            shBuffer[0] = sh; occlusionBuffer[0] = occlusion;
            renderer.GetPropertyBlock(lightingBlock);
            lightingBlock.CopySHCoefficientArraysFrom(shBuffer);
            lightingBlock.CopyProbeOcclusionArrayFrom(occlusionBuffer);
            renderer.SetPropertyBlock(lightingBlock);
            foreach (int i in materialBlocks)
            {
                renderer.GetPropertyBlock(lightingBlock, i);
                lightingBlock.CopySHCoefficientArraysFrom(shBuffer);
                lightingBlock.CopyProbeOcclusionArrayFrom(occlusionBuffer);
                renderer.SetPropertyBlock(lightingBlock, i);
            }
        }

        sealed class Settings
        {
            public readonly SphericalHarmonicsL2 AmbientProbe = RenderSettings.ambientProbe;
            public readonly Light Sun = RenderSettings.sun;
            readonly AmbientMode ambient = RenderSettings.ambientMode;
            readonly Color sky = RenderSettings.ambientSkyColor, equator = RenderSettings.ambientEquatorColor,
                ground = RenderSettings.ambientGroundColor, light = RenderSettings.ambientLight, fogColor = RenderSettings.fogColor;
            readonly float intensity = RenderSettings.ambientIntensity, reflectionIntensity = RenderSettings.reflectionIntensity,
                density = RenderSettings.fogDensity, start = RenderSettings.fogStartDistance, end = RenderSettings.fogEndDistance;
            readonly DefaultReflectionMode reflection = RenderSettings.defaultReflectionMode;
            readonly int bounces = RenderSettings.reflectionBounces, shaderLOD = Shader.globalMaximumLOD;
            readonly Texture custom = RenderSettings.customReflectionTexture;
            Texture defaultTexture = ReflectionProbe.defaultTexture;
            readonly bool fog = RenderSettings.fog;
            readonly FogMode fogMode = RenderSettings.fogMode;
            readonly Material skybox = RenderSettings.skybox;
            public void ApplyLOD() { Shader.globalMaximumLOD = shaderLOD; }
            public void FreezeReflection(Func<Texture, Vector4, Texture> freeze)
            { defaultTexture = freeze(defaultTexture, ReflectionProbe.defaultTextureHDRDecodeValues); }
            public void Apply()
            {
                RenderSettings.ambientMode = ambient; RenderSettings.ambientSkyColor = sky;
                RenderSettings.ambientEquatorColor = equator; RenderSettings.ambientGroundColor = ground;
                RenderSettings.ambientLight = light; RenderSettings.ambientIntensity = intensity;
                RenderSettings.ambientProbe = AmbientProbe; RenderSettings.sun = Sun;
                RenderSettings.defaultReflectionMode = reflection; RenderSettings.customReflectionTexture = custom;
                RenderSettings.reflectionIntensity = reflectionIntensity; RenderSettings.reflectionBounces = bounces;
                RenderSettings.skybox = skybox;
                RenderSettings.fog = fog; RenderSettings.fogColor = fogColor; RenderSettings.fogMode = fogMode;
                RenderSettings.fogDensity = density; RenderSettings.fogStartDistance = start; RenderSettings.fogEndDistance = end;
                Shader.globalMaximumLOD = shaderLOD;
            }
            public void ApplyCapturedReflection()
            {
                if (!defaultTexture) return;
                RenderSettings.customReflectionTexture = defaultTexture;
                RenderSettings.defaultReflectionMode = DefaultReflectionMode.Custom;
                RenderSettings.reflectionIntensity = 1; // baked into the decoded HDR pixels
            }
        }

        public void Dispose()
        {
            if (disposed) return; disposed = true; Ready = false;
            foreach (var copy in lights) if (copy.Light) Entry.DestroyOwned(copy.Light.gameObject);
            foreach (var copy in probes) if (copy.Probe) Entry.DestroyOwned(copy.Probe.gameObject);
            lights.Clear(); probes.Clear(); harmonics.Clear(); probePositions.Clear(); probeOcclusion.Clear();
            sampledRenderers.Clear(); sampledMeshes.Clear(); layers.Clear();
            rotatedLighting.Clear();
            foreach (var frozen in frozenTextures)
            {
                var texture = frozen.Copy;
                if (texture is RenderTexture rt) rt.Release();
                Entry.DestroyOwned(texture);
            }
            frozenTextures.Clear();
            if (Scene.IsValid() && Scene.isLoaded) SceneManager.UnloadSceneAsync(Scene);
        }
    }

    internal static class Harmonics
    {
        static readonly Vector3[] sampleDirections = new Vector3[9];
        static readonly Color[] sampleColors = new Color[9];
        static readonly Vector3[] directions = { Vector3.right, Vector3.left, Vector3.up, Vector3.down,
            Vector3.forward, Vector3.back, new Vector3(1, 1, 0).normalized,
            new Vector3(1, 0, 1).normalized, new Vector3(0, 1, 1).normalized };
        static readonly double[,] inverse = MakeInverse();
        static double[,] MakeInverse()
        {
            var matrix = new double[9, 18]; var colors = new Color[9];
            for (int column = 0; column < 9; ++column)
            {
                var basis = new SphericalHarmonicsL2(); basis[0, column] = 1;
                basis.Evaluate(directions, colors);
                for (int row = 0; row < 9; ++row) matrix[row, column] = colors[row].r;
                matrix[column, 9 + column] = 1;
            }
            for (int column = 0; column < 9; ++column)
            {
                int pivot = column;
                for (int row = column + 1; row < 9; ++row)
                    if (Math.Abs(matrix[row, column]) > Math.Abs(matrix[pivot, column])) pivot = row;
                if (Math.Abs(matrix[pivot, column]) < 1e-8) throw new InvalidOperationException("SH basis is singular");
                for (int k = 0; k < 18; ++k) { double t = matrix[column, k]; matrix[column, k] = matrix[pivot, k]; matrix[pivot, k] = t; }
                double scale = matrix[column, column];
                for (int k = 0; k < 18; ++k) matrix[column, k] /= scale;
                for (int row = 0; row < 9; ++row)
                {
                    if (row == column) continue; double factor = matrix[row, column];
                    for (int k = 0; k < 18; ++k) matrix[row, k] -= factor * matrix[column, k];
                }
            }
            var result = new double[9, 9];
            for (int i = 0; i < 9; ++i) for (int j = 0; j < 9; ++j) result[i, j] = matrix[i, j + 9];
            return result;
        }
        public static SphericalHarmonicsL2 Rotate(SphericalHarmonicsL2 source, Quaternion rotation)
        {
            if (Quaternion.Angle(rotation, Quaternion.identity) < .001f) return source;
            var inverseRotation = Quaternion.Inverse(rotation);
            for (int i = 0; i < 9; ++i) sampleDirections[i] = inverseRotation * directions[i];
            source.Evaluate(sampleDirections, sampleColors);
            var result = new SphericalHarmonicsL2();
            for (int channel = 0; channel < 3; ++channel)
                for (int i = 0; i < 9; ++i)
                {
                    double value = 0;
                    for (int j = 0; j < 9; ++j) value += inverse[i, j] * sampleColors[j][channel];
                    result[channel, i] = (float)value;
                }
            return result;
        }
    }
}
