using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace FTRez.LobbyModel
{
    // One startup probe using only 8x8 offscreen targets. No screen capture.
    // Exercise the exact bundled shader before installing camera callbacks.
    internal static class ShaderProbe
    {
        internal static void Verify(Shader shader)
        {
            var owned = new List<Object>();
            var previousTarget = RenderTexture.active;
            var oldView = Shader.GetGlobalMatrix("unity_MatrixV");
            var oldProjection = Shader.GetGlobalMatrix("glstate_matrix_projection");
            var oldZ = Shader.GetGlobalVector("_ZBufferParams");
            var oldOrtho = Shader.GetGlobalVector("unity_OrthoParams");
            var oldProjectionParams = Shader.GetGlobalVector("_ProjectionParams");
            CommandBuffer command = null;
            try
            {
                var material = new Material(shader); owned.Add(material);
                for (int i = 0; i < 3; ++i)
                {
                    string name = i == 0 ? "CoverageDepth" : i == 1 ? "SelfBeforeUI" : "TransparentForegroundDepth";
                    if (material.FindPass(name) != i) throw new InvalidOperationException("Pass order: " + name);
                }
                var quad = new Mesh(); owned.Add(quad);
                quad.vertices = new[] { new Vector3(-1, -1, -.5f), new Vector3(1, -1, -.5f),
                    new Vector3(1, 1, -.5f), new Vector3(-1, 1, -.5f) };
                quad.uv = new[] { Vector2.zero, Vector2.right, Vector2.one, Vector2.up };
                quad.triangles = new[] { 0, 1, 2, 0, 2, 3 };
                // Opaque material alpha must not hide its depth-covered pixels.
                var scene = Texture(new Color(.1f, .2f, .3f, .35f), owned);
                var lobby = Texture(new Color(.7f, .4f, .2f, 1), owned);
                var depth = Texture(new Color(.5f, .5f, .5f, 1), owned);
                var foreground = Texture(Color.white, owned);
                var coverage = Target(RenderTextureFormat.RGFloat, owned);
                var color = Target(RenderTextureFormat.ARGBHalf, owned);
                var barrier = Target(RenderTextureFormat.RFloat, owned);
                material.SetTexture("_MainTex", scene);
                material.SetTexture("_LobbyColor", lobby);
                material.SetTexture("_CameraDepthTexture", depth);
                material.SetTexture("_LobbyCoverage", coverage);
                material.SetTexture("_LobbyForeground", foreground);
                var pixels = new Texture2D(1, 1, TextureFormat.RGBAFloat, false, true); owned.Add(pixels);
                var values = new Color[5];
                for (int step = 0; step < 5; ++step)
                {
                    if (step == 2) material.SetTexture("_LobbyForeground", Texture2D.blackTexture);
                    if (step == 4) material.SetTexture("_CameraDepthTexture",
                        SystemInfo.usesReversedZBuffer ? Texture2D.blackTexture : Texture2D.whiteTexture);
                    var target = step == 0 || step == 4 ? coverage : step == 3 ? barrier : color;
                    command = new CommandBuffer { name = "FTRez bundled shader probe" };
                    command.SetRenderTarget(target);
                    command.ClearRenderTarget(true, true, step == 1 || step == 2 ? new Color(.1f, .2f, .3f, .6f) : Color.magenta);
                    command.SetViewProjectionMatrices(Matrix4x4.identity, Matrix4x4.Scale(new Vector3(1, 1, -1)));
                    command.SetGlobalVector("_ZBufferParams", new Vector4(0, 0, 0, 2));
                    command.SetGlobalVector("unity_OrthoParams", new Vector4(0, 0, 0, 1));
                    command.SetGlobalVector("_ProjectionParams", new Vector4(1, 0, 1, 1));
                    command.DrawMesh(quad, Matrix4x4.identity, material, 0, step == 0 || step == 4 ? 0 : step == 3 ? 2 : 1);
                    command.SetViewProjectionMatrices(oldView, oldProjection);
                    command.SetGlobalVector("_ZBufferParams", oldZ);
                    command.SetGlobalVector("unity_OrthoParams", oldOrtho);
                    command.SetGlobalVector("_ProjectionParams", oldProjectionParams);
                    Graphics.ExecuteCommandBuffer(command);
                    command.Release(); command = null;
                    RenderTexture.active = target;
                    pixels.ReadPixels(new Rect(4, 4, 1, 1), 0, 0); pixels.Apply();
                    values[step] = pixels.GetPixel(0, 0);
                }
                Near(values[0].r, 1, "opaque alpha .35 coverage"); Near(values[0].g, .5f, "coverage depth");
                Near(values[4].r, 0, "empty depth coverage");
                Near(values[1].r, .7f, "visible composite R"); Near(values[1].g, .4f, "visible composite G");
                Near(values[1].a, .6f, "visible destination alpha");
                Near(values[2].r, .1f, "occluded composite R"); Near(values[2].b, .3f, "occluded composite B");
                Near(values[2].a, .6f, "occluded destination alpha");
                Near(values[3].r, .5f, "foreground depth");
                VerifyReflection(shader);
                Debug.Log("[FTREZ-LOBBY] " + Entry.Revision + " bundled shader GPU probe PASS; graphics=" +
                    SystemInfo.graphicsDeviceType + " coverage=" + values[0].ToString("G5") +
                    " visible=" + values[1].ToString("G5") + " occluded=" + values[2].ToString("G5") +
                    " foreground=" + values[3].r);
            }
            catch (Exception error)
            {
                throw new InvalidOperationException("Lobby init [GPU_PROBE]: " + error.Message, error);
            }
            finally
            {
                if (command != null) command.Release();
                Shader.SetGlobalVector("_ZBufferParams", oldZ);
                Shader.SetGlobalVector("unity_OrthoParams", oldOrtho);
                Shader.SetGlobalVector("_ProjectionParams", oldProjectionParams);
                RenderTexture.active = previousTarget;
                foreach (var value in owned)
                {
                    if (value is RenderTexture texture) texture.Release();
                    Entry.DestroyOwned(value);
                }
            }
        }

        internal static void VerifyReflection(Shader shader)
        {
            var previous = RenderTexture.active;
            var cube = new Cubemap(8, TextureFormat.RGBAHalf, true) { filterMode = FilterMode.Point };
            RenderTexture copy = null;
            Cubemap frozen = null;
            try
            {
                // Asymmetric colours per face, x/y and mip catch rotations,
                // vertical flips, regenerated mips, HDR clipping and RGBM loss.
                for (int mip = 0; mip < cube.mipmapCount; ++mip)
                {
                    int size = Math.Max(1, 8 >> mip); var values = new Color[size * size];
                    for (int face = 0; face < 6; ++face)
                    {
                        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
                            values[y * size + x] = new Color(.2f + face * .3f, .1f + x * .1f, .2f + y * .07f + mip * .3f, .5f);
                        cube.SetPixels(values, (CubemapFace)face, mip);
                    }
                }
                cube.Apply(false, false);
                copy = ReflectionSnapshot.Freeze(cube, new Vector4(8, 2, 0, 1), shader);
                frozen = ReflectionSnapshot.ToCubemap(copy);
                for (int mip = 0; mip < cube.mipmapCount; ++mip)
                {
                    int size = Math.Max(1, 8 >> mip);
                    var facePixels = RenderTexture.GetTemporary(size, size, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
                    // Read the whole surface into a matching-size CPU texture.
                    // GLES partial readback into the reused 8-wide texture can
                    // pack mip rows at 4-wide strides (row 1 then reads row 2).
                    var pixels = new Texture2D(size, size, TextureFormat.RGBAFloat, false, true);
                    try
                    {
                    for (int face = 0; face < 6; ++face)
                    {
                        // ReadPixels addresses mip zero of the active texture;
                        // copy the selected uncompressed face/mip into a flat RT.
                        Graphics.CopyTexture(frozen, face, mip, facePixels, 0, 0);
                        RenderTexture.active = facePixels;
                        pixels.ReadPixels(new Rect(0, 0, size, size), 0, 0); pixels.Apply();
                        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
                        {
                            var actual = pixels.GetPixel(x, y);
                            var expected = cube.GetPixel((CubemapFace)face, x, y, mip) * 2;
                            string label = "reflection face=" + face + " mip=" + mip + " x=" + x + " y=" + y;
                            Near(actual.r, expected.r, label + " R");
                            Near(actual.g, expected.g, label + " G");
                            Near(actual.b, expected.b, label + " B");
                        }
                    }
                    }
                    finally
                    {
                        RenderTexture.active = previous;
                        RenderTexture.ReleaseTemporary(facePixels);
                        Entry.DestroyOwned(pixels);
                    }
                }
                Debug.Log("[FTREZ-LOBBY] HDR reflection snapshot GPU probe PASS; 6 faces, 4 authored mips, RGBM decode and orientation");
            }
            finally
            {
                RenderTexture.active = previous;
                if (copy) { copy.Release(); Entry.DestroyOwned(copy); }
                Entry.DestroyOwned(frozen);
                Entry.DestroyOwned(cube);
            }
        }

        static Texture2D Texture(Color color, List<Object> owned)
        {
            var texture = new Texture2D(1, 1, TextureFormat.RGBA32, false, true); owned.Add(texture);
            texture.SetPixel(0, 0, color); texture.Apply(); return texture;
        }

        static RenderTexture Target(RenderTextureFormat format, List<Object> owned)
        {
            var texture = new RenderTexture(8, 8, 24, format, RenderTextureReadWrite.Linear); owned.Add(texture);
            texture.filterMode = FilterMode.Point;
            if (!texture.Create()) throw new InvalidOperationException("Probe target unavailable: " + format);
            return texture;
        }

        static void Near(float value, float expected, string label)
        {
            if (float.IsNaN(value) || float.IsInfinity(value) || Mathf.Abs(value - expected) > .012f)
                throw new InvalidOperationException(label + " expected=" + expected + " actual=" + value);
        }
    }
}
