using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace FTRez.LobbyModel
{
    // A per-renderer lighting snapshot. Draws, depth, alpha, skinning and sorting
    // remain owned by the original camera. No camera render or screen mask here.
    internal sealed class DirectLighting : IDisposable
    {
        internal sealed class Sample
        {
            public Renderer Renderer;
            public Mesh Mesh;
            public Vector3 LocalPosition;
            public SphericalHarmonicsL2 SH;
            public Vector4 Occlusion, MainColor;
            public Texture Reflection;
        }
        sealed class Frozen
        {
            public Texture Source, Copy;
            public Vector4 Decode;
        }
        readonly List<Sample> samples = new List<Sample>();
        readonly List<Frozen> frozen = new List<Frozen>();
        readonly Quaternion rotation;
        readonly Vector3 mainDirection;
        readonly ShaderState globals;
        readonly SphericalHarmonicsL2[] sh = new SphericalHarmonicsL2[1];
        readonly Vector4[] occlusion = new Vector4[1];
        static readonly Vector4 decodedHDR = new Vector4(1, 1, 0, 0);
        static readonly int P_MainLightPosition = Shader.PropertyToID("_MainLightPosition");
        static readonly int P_MainLightColor = Shader.PropertyToID("_MainLightColor");
        static readonly int P_MainLightLayerMask = Shader.PropertyToID("_MainLightLayerMask");
        static readonly int Punity_LightData = Shader.PropertyToID("unity_LightData");
        static readonly int P_AdditionalLightsCount = Shader.PropertyToID("_AdditionalLightsCount");
        static readonly int P_MainLightShadowParams = Shader.PropertyToID("_MainLightShadowParams");
        static readonly int P_MainLightOcclusionProbes = Shader.PropertyToID("_MainLightOcclusionProbes");
        static readonly int P_AmbientOcclusionParam = Shader.PropertyToID("_AmbientOcclusionParam");
        static readonly int P_MainLightCookieTexture = Shader.PropertyToID("_MainLightCookieTexture");
        static readonly int Punity_FogParams = Shader.PropertyToID("unity_FogParams");
        static readonly int Punity_SpecCube0 = Shader.PropertyToID("unity_SpecCube0");
        static readonly int Punity_SpecCube1 = Shader.PropertyToID("unity_SpecCube1");
        static readonly int P_GlossyEnvironmentCubeMap = Shader.PropertyToID("_GlossyEnvironmentCubeMap");
        static readonly int Punity_SpecCube0_HDR = Shader.PropertyToID("unity_SpecCube0_HDR");
        static readonly int Punity_SpecCube1_HDR = Shader.PropertyToID("unity_SpecCube1_HDR");
        static readonly int P_GlossyEnvironmentCubeMap_HDR = Shader.PropertyToID("_GlossyEnvironmentCubeMap_HDR");
        static readonly int Punity_SpecCube0_ProbePosition = Shader.PropertyToID("unity_SpecCube0_ProbePosition");
        static readonly int Punity_SpecCube1_ProbePosition = Shader.PropertyToID("unity_SpecCube1_ProbePosition");
        static readonly int Punity_SpecCube0_BoxMin = Shader.PropertyToID("unity_SpecCube0_BoxMin");
        static readonly int Punity_SpecCube1_BoxMin = Shader.PropertyToID("unity_SpecCube1_BoxMin");
        static readonly int Punity_SpecCube0_BoxMax = Shader.PropertyToID("unity_SpecCube0_BoxMax");
        static readonly int Punity_SpecCube1_BoxMax = Shader.PropertyToID("unity_SpecCube1_BoxMax");
        public int Count { get { return samples.Count; } }

        public DirectLighting(Transform host, Renderer[] renderers, ref RenderingData data)
        {
            rotation = host.rotation;
            globals = new ShaderState();
            var lights = data.lightData.visibleLights;
            int main = data.lightData.mainLightIndex;
            UniversalRenderPipeline.InitializeLightConstants_Common(lights, main,
                out var direction, out var color, out _, out _, out _);
            mainDirection = direction;
            var closest = new List<ReflectionProbeBlendInfo>();
            try
            {
                foreach (var renderer in renderers)
                {
                    if (!renderer) continue;
                    Vector3 position = renderer.probeAnchor ? renderer.probeAnchor.position : renderer.bounds.center;
                    LightProbes.CalculateInterpolatedLightAndOcclusionProbes(new[] { position }, sh, occlusion);
                    var sample = new Sample { Renderer = renderer, Mesh = MeshOf(renderer),
                        LocalPosition = host.InverseTransformPoint(position), SH = sh[0], Occlusion = occlusion[0],
                        MainColor = main >= 0 && Affects(lights[main].light, renderer, data.lightData.supportsLightLayers) ? color : Vector4.zero };
                    // Extra lobby lights become a fixed diffuse probe. This works
                    // even when the world camera disables additional-light variants.
                    // Their specular/cookies/shadows are deliberately not replayed.
                    for (int i = 0; i < lights.Length; ++i)
                    {
                        if (i == main || !Affects(lights[i].light, renderer, data.lightData.supportsLightLayers)) continue;
                        UniversalRenderPipeline.InitializeLightConstants_Common(lights, i,
                            out var pos, out var energy, out var attenuation, out var spot, out _);
                        Vector3 vector = (Vector3)pos - position * pos.w;
                        float distance = Mathf.Max(vector.sqrMagnitude, .0001f);
                        Vector3 dir = vector / Mathf.Sqrt(distance);
                        float strength = 1;
                        if (pos.w != 0)
                        {
                            float factor = distance * attenuation.x;
                            float smooth = Mathf.Clamp01(1 - factor * factor);
                            float cone = Mathf.Clamp01(Vector3.Dot(spot, dir) * attenuation.z + attenuation.w);
                            strength = smooth * smooth * cone * cone / distance;
                        }
                        sample.SH.AddDirectionalLight(dir, (Color)energy, strength);
                    }
                    closest.Clear(); renderer.GetClosestReflectionProbes(closest);
                    ReflectionProbe probe = null; float weight = 0;
                    foreach (var item in closest)
                        if (item.probe && item.probe.texture && item.weight > weight)
                        { probe = item.probe; weight = item.weight; }
                    sample.Reflection = probe ? Freeze(probe.texture, probe.textureHDRDecodeValues) :
                        Freeze(ReflectionProbe.defaultTexture, ReflectionProbe.defaultTextureHDRDecodeValues);
                    samples.Add(sample);
                }
                if (samples.Count == 0) throw new InvalidOperationException("Lobby has no model renderers");
                Debug.Log("[FTREZ-LOBBY] direct snapshot parts=" + samples.Count + " main=" + main +
                    " visibleLights=" + lights.Length + " reflectionTextures=" + frozen.Count +
                    " extraLights=diffuse-probe post=world-camera");
            }
            catch { Dispose(); throw; }
        }

        static bool Affects(Light light, Renderer renderer, bool lightLayers)
        {
            if (!light) return true;
            if ((light.cullingMask & (1 << renderer.gameObject.layer)) == 0) return false;
            var extra = lightLayers ? light.GetComponent<UniversalAdditionalLightData>() : null;
            return !extra || (extra.renderingLayers & renderer.renderingLayerMask) != 0;
        }

        static Mesh MeshOf(Renderer renderer)
        {
            if (renderer is SkinnedMeshRenderer skin) return skin.sharedMesh;
            var filter = renderer.GetComponent<MeshFilter>(); return filter ? filter.sharedMesh : null;
        }

        Texture Freeze(Texture source, Vector4 decode)
        {
            if (!source) return null;
            foreach (var item in frozen) if (item.Source == source && item.Decode == decode) return item.Copy;
            var rt = ReflectionSnapshot.Freeze(source, decode, Entry.SnapshotShader);
            Texture copy;
            try { copy = ReflectionSnapshot.ToCubemap(rt); }
            finally { rt.Release(); Entry.DestroyOwned(rt); }
            frozen.Add(new Frozen { Source = source, Copy = copy, Decode = decode });
            return copy;
        }

        Sample Select(Transform host, Renderer renderer)
        {
            foreach (var sample in samples) if (sample.Renderer == renderer) return sample;
            var mesh = MeshOf(renderer);
            var position = host.InverseTransformPoint(renderer.probeAnchor ? renderer.probeAnchor.position : renderer.bounds.center);
            Sample matched = null; float meshDistance = float.PositiveInfinity;
            if (mesh) foreach (var sample in samples) if (sample.Mesh == mesh)
            {
                float distance = (sample.LocalPosition - position).sqrMagnitude;
                if (distance < meshDistance) { meshDistance = distance; matched = sample; }
            }
            if (matched != null) return matched;
            Sample nearest = samples[0]; float best = float.PositiveInfinity;
            foreach (var sample in samples)
            {
                float distance = (sample.LocalPosition - position).sqrMagnitude;
                if (distance < best) { best = distance; nearest = sample; }
            }
            return nearest;
        }

        public Binding Bind(Transform host, Renderer[] renderers) { return new Binding(this, host, renderers); }

        public sealed class Binding
        {
            readonly DirectLighting snapshot;
            public readonly Transform Host;
            public readonly Renderer[] Renderers;
            readonly Sample[] samples;
            readonly SphericalHarmonicsL2[] rotated;
            Quaternion lastRotation;
            Quaternion delta;
            Vector3 direction;
            bool hasRotation;
            internal Binding(DirectLighting snapshot, Transform host, Renderer[] renderers)
            {
                this.snapshot = snapshot; Host = host; Renderers = (Renderer[])renderers.Clone();
                samples = new Sample[renderers.Length]; rotated = new SphericalHarmonicsL2[renderers.Length];
                for (int i = 0; i < renderers.Length; ++i)
                    if (renderers[i]) samples[i] = snapshot.Select(host, renderers[i]);
            }
            public bool Matches(Transform host, Renderer[] renderers)
            {
                if (Host != host || Renderers.Length != renderers.Length) return false;
                for (int i = 0; i < renderers.Length; ++i) if (Renderers[i] != renderers[i]) return false;
                return true;
            }
            public void Prepare()
            {
                delta = Host.rotation * Quaternion.Inverse(snapshot.rotation);
                if (!hasRotation || !lastRotation.Equals(delta))
                {
                    for (int i = 0; i < samples.Length; ++i)
                        if (samples[i] != null) rotated[i] = Harmonics.Rotate(samples[i].SH, delta);
                    lastRotation = delta; hasRotation = true;
                }
                direction = delta * snapshot.mainDirection;
            }
            public void Apply(int index, MaterialPropertyBlock block, bool customProbe)
            {
                var sample = samples[index];
                snapshot.globals.ApplyModel(block, delta);
                block.SetVector(P_MainLightPosition, direction);
                block.SetVector(P_MainLightColor, sample.MainColor);
                block.SetInteger(P_MainLightLayerMask, -1);
                block.SetVector(Punity_LightData, new Vector4(0, 0, 1, 0));
                block.SetVector(P_AdditionalLightsCount, Vector4.zero);
                block.SetVector(P_MainLightShadowParams, new Vector4(0, 0, 0, 1));
                block.SetVector(P_MainLightOcclusionProbes, Vector4.zero);
                block.SetVector(P_AmbientOcclusionParam, Vector4.zero);
                block.SetTexture(P_MainLightCookieTexture, Texture2D.whiteTexture);
                // Prevent world fog from creating angle/distance dependent holes
                // in the fixed model lighting; the camera's post still runs once.
                block.SetVector(Punity_FogParams, new Vector4(0, 0, 0, 1));
                if (!customProbe)
                {
                    snapshot.sh[0] = rotated[index]; snapshot.occlusion[0] = sample.Occlusion;
                    block.CopySHCoefficientArraysFrom(snapshot.sh);
                    block.CopyProbeOcclusionArrayFrom(snapshot.occlusion);
                }
                if (sample.Reflection)
                {
                    // Keep a stable dominant lobby probe, with no world box
                    // projection or position-driven switching between probes.
                    var center = Renderers[index].bounds.center;
                    var min = center - Vector3.one * 10; var max = center + Vector3.one * 10;
                    block.SetTexture(Punity_SpecCube0, sample.Reflection);
                    block.SetTexture(Punity_SpecCube1, sample.Reflection);
                    block.SetTexture(P_GlossyEnvironmentCubeMap, sample.Reflection);
                    block.SetVector(Punity_SpecCube0_HDR, decodedHDR);
                    block.SetVector(Punity_SpecCube1_HDR, decodedHDR);
                    block.SetVector(P_GlossyEnvironmentCubeMap_HDR, decodedHDR);
                    block.SetVector(Punity_SpecCube0_ProbePosition, Vector4.zero);
                    block.SetVector(Punity_SpecCube1_ProbePosition, Vector4.zero);
                    block.SetVector(Punity_SpecCube0_BoxMin, new Vector4(min.x, min.y, min.z, 1));
                    block.SetVector(Punity_SpecCube1_BoxMin, new Vector4(min.x, min.y, min.z, 1));
                    block.SetVector(Punity_SpecCube0_BoxMax, new Vector4(max.x, max.y, max.z, 1));
                    block.SetVector(Punity_SpecCube1_BoxMax, new Vector4(max.x, max.y, max.z, 1));
                }
            }
        }

        // Reused per camera stack depth. Save both renderer and indexed blocks;
        // an indexed MPB takes precedence over the renderer-level MPB in Unity.
        internal sealed class Lease
        {
            sealed class Part
            {
                public Renderer Renderer;
                public readonly MaterialPropertyBlock Block = new MaterialPropertyBlock();
                public readonly List<MaterialPropertyBlock> Slots = new List<MaterialPropertyBlock>();
                public readonly List<int> Used = new List<int>();
                public LightProbeUsage Probe;
                public int Lightmap, RealtimeLightmap;
                public bool Saved;
            }
            readonly List<Part> parts = new List<Part>();
            readonly List<Material> materials = new List<Material>();
            readonly MaterialPropertyBlock work = new MaterialPropertyBlock();
            int count;
            public void Apply(Binding binding)
            {
                try
                {
                    binding.Prepare();
                    for (int i = 0; i < binding.Renderers.Length; ++i)
                    {
                        var renderer = binding.Renderers[i]; if (!renderer) continue;
                        if (parts.Count <= count) parts.Add(new Part());
                        var part = parts[count++]; part.Renderer = renderer;
                        part.Probe = renderer.lightProbeUsage; part.Lightmap = renderer.lightmapIndex;
                        part.RealtimeLightmap = renderer.realtimeLightmapIndex;
                        renderer.GetPropertyBlock(part.Block); part.Used.Clear();
                        renderer.GetSharedMaterials(materials);
                        while (part.Slots.Count < materials.Count) part.Slots.Add(new MaterialPropertyBlock());
                        for (int slot = 0; slot < materials.Count; ++slot)
                        {
                            renderer.GetPropertyBlock(part.Slots[slot], slot);
                            if (!part.Slots[slot].isEmpty) part.Used.Add(slot);
                        }
                        part.Saved = true;
                        renderer.lightProbeUsage = LightProbeUsage.CustomProvided;
                        renderer.lightmapIndex = renderer.realtimeLightmapIndex = -1;
                        renderer.GetPropertyBlock(work);
                        binding.Apply(i, work, part.Probe == LightProbeUsage.CustomProvided);
                        renderer.SetPropertyBlock(work);
                        foreach (int slot in part.Used)
                        {
                            renderer.GetPropertyBlock(work, slot);
                            binding.Apply(i, work, part.Probe == LightProbeUsage.CustomProvided);
                            renderer.SetPropertyBlock(work, slot);
                        }
                    }
                }
                catch { Restore(); throw; }
            }
            public void Restore()
            {
                for (int i = count - 1; i >= 0; --i)
                {
                    var part = parts[i]; var renderer = part.Renderer;
                    if (part.Saved && renderer)
                    {
                        renderer.SetPropertyBlock(part.Block.isEmpty ? null : part.Block);
                        foreach (int slot in part.Used) renderer.SetPropertyBlock(part.Slots[slot], slot);
                        renderer.lightProbeUsage = part.Probe; renderer.lightmapIndex = part.Lightmap;
                        renderer.realtimeLightmapIndex = part.RealtimeLightmap;
                    }
                    part.Saved = false; part.Renderer = null;
                }
                count = 0;
            }
        }

        public void Dispose()
        {
            foreach (var item in frozen) Entry.DestroyOwned(item.Copy);
            frozen.Clear(); samples.Clear();
        }
    }
}
