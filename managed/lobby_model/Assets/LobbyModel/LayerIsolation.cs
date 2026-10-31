using System;
using UnityEngine;

namespace FTRez.LobbyModel
{
    // Cache until a native creation/layer hook or scene event invalidates it.
    // Missing hooks and in-flight asynchronous instantiation retain full checks.
    internal sealed class LayerIsolation
    {
        readonly int[] aliases = new int[32];
        long generation = -1, scenes = -1;
        int previousMask, previousScene;
        public int RefreshCount { get; private set; }
        public int Mask { get; private set; }
        public void Refresh(int sourceMask, UnityEngine.SceneManagement.Scene ownedScene)
        {
            bool cacheable = GeometryChanges.Read(out long current, out long currentScenes);
            if (cacheable && generation == current && scenes == currentScenes &&
                previousMask == sourceMask && previousScene == ownedScene.handle) return;
            ++RefreshCount;
            uint used = 0;
            foreach (var item in UnityEngine.Object.FindObjectsByType(typeof(Renderer), FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                var renderer = (Renderer)item;
                if (renderer && renderer.gameObject.scene != ownedScene) used |= 1u << renderer.gameObject.layer;
            }
            Mask = 0;
            for (int i = 0; i < 32; ++i)
            {
                aliases[i] = -1;
                if ((sourceMask & (1 << i)) == 0) continue;
                for (int j = 30; j >= 8; --j)
                    if ((used & (1u << j)) == 0) { aliases[i] = j; used |= 1u << j; Mask |= 1 << j; break; }
                if (aliases[i] < 0) throw new InvalidOperationException("No free draw layer for lobby model");
            }
            generation = current; scenes = currentScenes;
            previousMask = sourceMask; previousScene = ownedScene.handle;
        }
        public int Layer(int original)
        {
            if (aliases[original] < 0) throw new InvalidOperationException("Uncaptured lobby model layer " + original);
            return aliases[original];
        }
        public void SetLayer(GameObject item, int layer)
        {
            if (item.layer == layer) return;
            bool stable = GeometryChanges.Read(out long before, out long beforeScenes);
            item.layer = layer;
            // The native set_layer hook increments exactly twice. A temporary
            // change owned by this scope does not occupy a new world layer.
            // Any other mutation/pending operation still invalidates the cache.
            if (stable && generation == before && scenes == beforeScenes &&
                GeometryChanges.Read(out long after, out long afterScenes) &&
                afterScenes == beforeScenes && after == before + 2) generation = after;
        }
        public int Map(int original)
        {
            int result = 0;
            for (int i = 0; i < 32; ++i)
                if (aliases[i] >= 0 && (original & (1 << i)) != 0) result |= 1 << aliases[i];
            return result;
        }
    }
}
