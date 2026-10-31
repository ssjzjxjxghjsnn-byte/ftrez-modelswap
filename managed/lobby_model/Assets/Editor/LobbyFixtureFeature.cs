using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

// Exercises real renderer-feature execution and serialized Material ownership.
public sealed class LobbyFixtureFeature : ScriptableRendererFeature
{
    public Material Tint;
    public bool GeometryOnly;
    public LayerMask Layers = -1;
    public static readonly HashSet<int> Live = new HashSet<int>();
    public static readonly List<Material> Generated = new List<Material>();
    Pass pass;
    public override void Create()
    {
        Live.Add(GetInstanceID());
        pass = new Pass(this);
    }
    public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData data)
    {
        if (Tint) renderer.EnqueuePass(pass);
    }
    protected override void Dispose(bool disposing) { Live.Remove(GetInstanceID()); }

    sealed class Pass : ScriptableRenderPass
    {
        readonly LobbyFixtureFeature owner;
        readonly Material generated;
        FilteringSettings filtering;
        public Pass(LobbyFixtureFeature owner)
        {
            this.owner = owner;
            // Match PostProcessOutLine: Create replaces a pass that allocates a
            // private Material, with no feature-side Material disposal.
            generated = new Material(Shader.Find("Hidden/FTRez/LobbyFixtureTint"))
                { hideFlags = HideFlags.HideAndDontSave };
            Generated.Add(generated);
            filtering = new FilteringSettings(RenderQueueRange.all, owner.Layers);
            renderPassEvent = RenderPassEvent.AfterRenderingOpaques;
            ConfigureInput(ScriptableRenderPassInput.Color);
        }
        public override void Execute(ScriptableRenderContext context, ref RenderingData data)
        {
            if (owner.GeometryOnly)
            {
                generated.CopyPropertiesFromMaterial(owner.Tint);
                var drawing = CreateDrawingSettings(new ShaderTagId("UniversalForward"), ref data, SortingCriteria.CommonOpaque);
                drawing.overrideMaterial = generated; drawing.overrideMaterialPassIndex = 1;
                context.DrawRenderers(data.cullResults, ref drawing, ref filtering);
                return;
            }
            var cmd = CommandBufferPool.Get("Lobby fixture tint");
            int copy = Shader.PropertyToID("_LobbyFixtureCopy");
            try
            {
                var descriptor = data.cameraData.cameraTargetDescriptor;
                descriptor.depthBufferBits = 0; descriptor.msaaSamples = 1;
                cmd.GetTemporaryRT(copy, descriptor);
                var color = data.cameraData.renderer.cameraColorTargetHandle.nameID;
                generated.CopyPropertiesFromMaterial(owner.Tint);
                cmd.Blit(color, copy);
                cmd.Blit(copy, color, generated);
                cmd.ReleaseTemporaryRT(copy);
                context.ExecuteCommandBuffer(cmd);
            }
            finally { CommandBufferPool.Release(cmd); }
        }
    }
}
