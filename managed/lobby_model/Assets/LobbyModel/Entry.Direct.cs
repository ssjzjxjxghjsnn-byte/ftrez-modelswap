using System;
using System.Collections.Generic;
using System.Diagnostics;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace FTRez.LobbyModel
{
    public static partial class Entry
    {
#if UNITY_EDITOR
        // Keep the previous algorithm executable only for historical GPU
        // regressions. The deployed helper always uses direct lighting.
        internal static bool ValidateLegacyComposition;
#endif
        static bool DirectRendering
        {
            get
            {
#if UNITY_EDITOR
                return !ValidateLegacyComposition;
#else
                return true;
#endif
            }
        }
        static DirectLighting directLighting;
        static readonly DirectLighting.Binding[] directBindings = new DirectLighting.Binding[3];
        sealed class DirectFrame
        {
            public Camera Camera;
            public readonly DirectLighting.Lease Lease = new DirectLighting.Lease();
        }
        static readonly List<DirectFrame> directFrames = new List<DirectFrame>();
        static int directDepth, directCalls, directModels;
        static double directMilliseconds;
        static float directNextTiming;

        static void SetDirectTargets(Camera sourceCamera, bool any, bool changed, bool sourceChanged)
        {
            lobbyCamera = lobbySource ? sourceCamera : null;
            if (lobbySource)
                captureRenderers = targets[0].Host == lobbySource && targets[0].Renderers.Length != 0 ?
                    targets[0].Renderers : lobbySource.GetComponentsInChildren<Renderer>(true);
            for (int i = 0; i < targets.Length; ++i)
                if (directBindings[i] != null && !directBindings[i].Matches(targets[i].Host, targets[i].Renderers))
                    directBindings[i] = null;
            if (!enabled) Report("Lobby lighting: off");
            else if (!any && !lobbySource) Report("Lobby lighting: waiting for models");
            else if (lobbySource && sourceChanged) Report("Lobby lighting: waiting for actual lobby");
            else if (directLighting == null) Report("Lobby lighting: waiting for a captured lobby");
            else if (changed) Report("Lobby lighting: baseline ready; direct model lighting");
        }

        static void BeginDirectCamera(Camera camera)
        {
            try
            {
                if (camera == lobbyCamera && lobbySource)
                {
                    if (Time.realtimeSinceStartup >= nextCapture)
                    {
                        captureFrame = Time.frameCount;
                        var data = camera.GetComponent<UniversalAdditionalCameraData>();
                        EnsureFeature(data ? data.scriptableRenderer : UniversalRenderPipeline.asset.scriptableRenderer, data);
                    }
                    return;
                }
                if (directLighting == null || !camera || camera.cameraType != CameraType.Game) return;
                long start = Stopwatch.GetTimestamp();
                DirectFrame frame = null;
                int count = 0;
                try
                {
                    for (int i = 0; i < targets.Length; ++i)
                    {
                        var target = targets[i];
                        if (!target.Host || target.Host == lobbySource) continue;
                        bool visible = false;
                        foreach (var renderer in target.Renderers)
                            if (Visible(renderer) && (camera.cullingMask & (1 << renderer.gameObject.layer)) != 0)
                            { visible = true; break; }
                        if (!visible) continue;
                        if (frame == null)
                        {
                            if (directFrames.Count <= directDepth) directFrames.Add(new DirectFrame());
                            frame = directFrames[directDepth++]; frame.Camera = camera;
                        }
                        if (directBindings[i] == null)
                            directBindings[i] = directLighting.Bind(target.Host, target.Renderers);
                        frame.Lease.Apply(directBindings[i]);
                        ++count;
                    }
                }
                catch
                {
                    if (frame != null) { frame.Lease.Restore(); frame.Camera = null; --directDepth; }
                    throw;
                }
                if (count == 0) return;
                directMilliseconds += (Stopwatch.GetTimestamp() - start) * 1000.0 / Stopwatch.Frequency;
                ++directCalls; directModels += count;
                lastCompositeTime = Time.realtimeSinceStartup;
                if (activeCount != count || activeGeneration != baselineGeneration)
                {
                    activeCount = count; activeGeneration = baselineGeneration;
                    activeMessage = "Lobby lighting: active on " + count + " model(s); direct; baseline " + baselineGeneration;
                }
                Report(activeMessage);
            }
            catch (Exception error) { Failure("direct lighting " + camera.name, error); }
        }

        static void EndDirectCamera(Camera camera)
        {
            long start = Stopwatch.GetTimestamp();
            for (int i = directDepth - 1; i >= 0; --i)
                if (directFrames[i].Camera == camera)
                {
                    while (directDepth > i)
                    {
                        var frame = directFrames[--directDepth];
                        frame.Lease.Restore(); frame.Camera = null;
                    }
                    directMilliseconds += (Stopwatch.GetTimestamp() - start) * 1000.0 / Stopwatch.Frequency;
                    break;
                }
            if (directCalls > 0 && Time.realtimeSinceStartup >= directNextTiming)
            {
                UnityEngine.Debug.Log("[FTREZ-LOBBY] direct lighting CPU apply+restore avg=" +
                    (directMilliseconds / directCalls).ToString("F3") + "ms cameras=" + directCalls +
                    " models=" + directModels + " privateRenders=0 composites=0 post=world-camera");
                directMilliseconds = 0; directCalls = directModels = 0;
                directNextTiming = Time.realtimeSinceStartup + 5;
            }
        }

        static void CaptureDirect(Camera camera, ref RenderingData data)
        {
            if (!lobbySource || camera != lobbyCamera || captureFrame != Time.frameCount) return;
            captureFrame = -1;
            try
            {
                var captured = new DirectLighting(lobbySource, captureRenderers, ref data);
                RestoreDirectFrames();
                var previous = directLighting; directLighting = captured;
                for (int i = 0; i < directBindings.Length; ++i) directBindings[i] = null;
                if (previous != null) previous.Dispose();
                ++baselineGeneration; activeGeneration = -1;
                nextCapture = Time.realtimeSinceStartup + 10;
                Report("Lobby lighting: captured actual lobby; " + captured.Count + " parts; direct lighting");
            }
            catch (Exception error) { nextCapture = Time.realtimeSinceStartup + 1; Failure("direct capture", error); }
        }

        static void RestoreDirectFrames()
        {
            while (directDepth > 0)
            {
                var frame = directFrames[--directDepth]; frame.Lease.Restore(); frame.Camera = null;
            }
        }

        static void ShutdownDirect()
        {
            RestoreDirectFrames(); directFrames.Clear();
            if (directLighting != null) directLighting.Dispose(); directLighting = null;
            for (int i = 0; i < directBindings.Length; ++i) directBindings[i] = null;
            directMilliseconds = 0; directCalls = directModels = 0; directNextTiming = 0;
        }
    }
}
