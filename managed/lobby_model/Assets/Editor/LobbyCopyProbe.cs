using System.Collections.Generic;
using System.Reflection;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

// Inspect the actual scheduled URP passes, not just requested input flags.
public sealed class LobbyCopyProbe : ScriptableRendererFeature
{
    public bool RequestColor;
    public static int PrivateCopies, WorldCopies, PrivateCalls, WorldCalls;
    ProbePass pass;
    public static void ResetCounts() { PrivateCopies = WorldCopies = PrivateCalls = WorldCalls = 0; }
    public override void Create() { pass = new ProbePass(); }
    public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData data)
    {
        pass.ConfigureInput(RequestColor ? ScriptableRenderPassInput.Color : ScriptableRenderPassInput.None);
        renderer.EnqueuePass(pass);
    }
    sealed class ProbePass : ScriptableRenderPass
    {
        static readonly PropertyInfo queue = typeof(ScriptableRenderer).GetProperty("activeRenderPassQueue",
            BindingFlags.Instance | BindingFlags.NonPublic);
        public ProbePass() { renderPassEvent = RenderPassEvent.BeforeRenderingPostProcessing; }
        public override void Execute(ScriptableRenderContext context, ref RenderingData data)
        {
            int copies = 0;
            foreach (var item in (List<ScriptableRenderPass>)queue.GetValue(data.cameraData.renderer))
                if (item.GetType().Name == "CopyColorPass") ++copies;
            if (data.cameraData.camera.name == "FTREZ.LobbyCamera") { PrivateCopies += copies; ++PrivateCalls; }
            else { WorldCopies += copies; ++WorldCalls; }
        }
    }
}
