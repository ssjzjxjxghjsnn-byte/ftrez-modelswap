using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace FTRez.LobbyModel
{
    // Borrow the retained lobby renderers for a synchronous camera submission.
    // No GameObject, renderer, mesh, material, bones or pose are copied or moved.
    internal sealed class OriginalModel : IDisposable
    {
        sealed class Part
        {
            public readonly MaterialPropertyBlock Block = new MaterialPropertyBlock();
            public readonly List<MaterialPropertyBlock> Blocks = new List<MaterialPropertyBlock>();
            public readonly List<int> Slots = new List<int>();
            public int Layer, Lightmap, RealtimeLightmap;
            public LightProbeUsage ProbeUsage;
            public bool Saved, LightingChanged;
        }
        readonly Part[] parts;
        readonly List<Material> materials = new List<Material>();
        LayerIsolation isolation;
        bool borrowed;
        public readonly Renderer[] Renderers;
        public readonly List<int>[] MaterialBlockSlots;

        public OriginalModel(Renderer[] source)
        {
            Renderers = (Renderer[])source.Clone();
            parts = new Part[source.Length];
            MaterialBlockSlots = new List<int>[source.Length];
            for (int i = 0; i < source.Length; ++i)
            {
                parts[i] = new Part(); MaterialBlockSlots[i] = parts[i].Slots;
            }
            Debug.Log("[FTREZ-LOBBY] direct original renderers=" + source.Length + " replicas=0");
        }
        public bool Matches(Renderer[] source)
        {
            if (source.Length != Renderers.Length) return false;
            for (int i = 0; i < source.Length; ++i) if (source[i] != Renderers[i]) return false;
            return true;
        }
        public LightProbeUsage SourceProbeUsage(int index) { return parts[index].ProbeUsage; }

        public void Borrow(LayerIsolation layers)
        {
            if (borrowed) throw new InvalidOperationException("Original model already in use");
            borrowed = true; isolation = layers;
            try
            {
                for (int i = 0; i < Renderers.Length; ++i)
                {
                    var renderer = Renderers[i]; var part = parts[i];
                    if (!renderer) continue;
                    part.Layer = renderer.gameObject.layer;
                    part.ProbeUsage = renderer.lightProbeUsage;
                    part.Lightmap = renderer.lightmapIndex; part.RealtimeLightmap = renderer.realtimeLightmapIndex;
                    part.Slots.Clear(); part.Block.Clear();
                    if (renderer.HasPropertyBlock())
                    {
                        renderer.GetPropertyBlock(part.Block);
                        renderer.GetSharedMaterials(materials);
                        while (part.Blocks.Count < materials.Count) part.Blocks.Add(new MaterialPropertyBlock());
                        for (int slot = 0; slot < materials.Count; ++slot)
                        {
                            renderer.GetPropertyBlock(part.Blocks[slot], slot);
                            if (!part.Blocks[slot].isEmpty) part.Slots.Add(slot);
                        }
                    }
                    part.Saved = true;
                    // Only illumination state is borrowed; Unity keeps the
                    // original renderer's skinning, sorting and other flags.
                    renderer.lightmapIndex = renderer.realtimeLightmapIndex = -1;
                }
            }
            catch { Restore(); throw; }
        }
        public void PrepareLighting(int index)
        {
            parts[index].LightingChanged = true;
            Renderers[index].lightProbeUsage = LightProbeUsage.CustomProvided;
        }
        public void Restore()
        {
            if (!borrowed) return;
            for (int i = 0; i < Renderers.Length; ++i)
            {
                var renderer = Renderers[i]; var part = parts[i];
                if (!part.Saved) continue;
                if (renderer)
                {
                    isolation.SetLayer(renderer.gameObject, part.Layer);
                    renderer.lightProbeUsage = part.ProbeUsage;
                    renderer.lightmapIndex = part.Lightmap; renderer.realtimeLightmapIndex = part.RealtimeLightmap;
                    if (part.LightingChanged)
                    {
                        renderer.SetPropertyBlock(part.Block.isEmpty ? null : part.Block);
                        foreach (int slot in part.Slots) renderer.SetPropertyBlock(part.Blocks[slot], slot);
                    }
                }
                part.Saved = part.LightingChanged = false;
            }
            borrowed = false; isolation = null;
        }
        public void Dispose() { Restore(); }
    }
}
