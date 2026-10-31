using System;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

namespace FTRez.LobbyModel
{
    internal static class ReflectionSnapshot
    {
        public static Cubemap ToCubemap(RenderTexture source)
        {
            // Store the frozen pixels in a Cubemap, separate from the temporary
            // render target used for decoding the source texture.
            if ((SystemInfo.copyTextureSupport & CopyTextureSupport.RTToTexture) == 0)
                throw new InvalidOperationException("HDR reflection RT-to-cubemap copy unsupported");
            var cube = new Cubemap(source.width, source.graphicsFormat,
                source.mipmapCount > 1 ? TextureCreationFlags.MipChain : TextureCreationFlags.None, source.mipmapCount)
                { hideFlags = HideFlags.HideAndDontSave, name = "FTREZ.Lobby.FrozenReflection",
                  filterMode = source.filterMode, wrapMode = TextureWrapMode.Clamp };
            try
            {
                cube.Apply(false, true); // allocate its GL texture before the GPU copy
                Graphics.CopyTexture(source, cube); // identical, uncompressed RGBAHalf formats
                return cube;
            }
            catch { Entry.DestroyOwned(cube); throw; }
        }
        // Sample the engine texture (including its platform decompression),
        // decode its actual HDR encoding, and retain every authored mip. Never
        // CopyTexture an unsupported ASTC storage format or regenerate roughness.
        public static RenderTexture Freeze(Texture source, Vector4 decode, Shader shader)
        {
            if (!source || source.dimension != TextureDimension.Cube)
                throw new InvalidOperationException("Lobby reflection must be a cubemap");
            const GraphicsFormat format = GraphicsFormat.R16G16B16A16_SFloat;
            if (!SystemInfo.IsFormatSupported(format, FormatUsage.Render))
                throw new InvalidOperationException("HDR reflection snapshot format unavailable");
            int mips = source.mipmapCount;
            var target = new RenderTexture(new RenderTextureDescriptor(source.width, source.height, format, 0) {
                dimension = TextureDimension.Cube, volumeDepth = 1, msaaSamples = 1,
                mipCount = mips, useMipMap = mips > 1, autoGenerateMips = false
            }) { hideFlags = HideFlags.HideAndDontSave, name = "FTREZ.Lobby.ReflectionSnapshot",
                filterMode = source.filterMode, wrapMode = TextureWrapMode.Clamp, anisoLevel = source.anisoLevel };
            Material material = null;
            var command = new CommandBuffer { name = "FTREZ freeze lobby reflection" };
            try
            {
                if (!target.Create()) throw new InvalidOperationException("Cannot allocate lobby HDR reflection");
                material = new Material(shader) { hideFlags = HideFlags.HideAndDontSave };
                int pass = material.FindPass("ReflectionSnapshot");
                if (pass < 0) throw new InvalidOperationException("Reflection snapshot shader pass missing");
                material.SetTexture("_SnapshotCube", source);
                material.SetVector("_SnapshotDecode", decode);
                for (int mip = 0; mip < mips; ++mip)
                    for (int face = 0; face < 6; ++face)
                    {
                        var block = new MaterialPropertyBlock();
                        block.SetVector("_SnapshotFaceMip", new Vector4(face, mip, 0, 0));
                        command.SetRenderTarget(target, mip, (CubemapFace)face);
                        command.SetViewport(new Rect(0, 0, Mathf.Max(1, source.width >> mip), Mathf.Max(1, source.height >> mip)));
                        command.DrawProcedural(Matrix4x4.identity, material, pass, MeshTopology.Triangles, 3, 1, block);
                    }
                Graphics.ExecuteCommandBuffer(command);
                Debug.Log("[FTREZ-LOBBY] reflection sampled " + source.graphicsFormat + " -> " + target.graphicsFormat +
                    " size=" + source.width + " mips=" + mips + " decode=" + decode);
                return target;
            }
            catch { target.Release(); Entry.DestroyOwned(target); throw; }
            finally { command.Release(); Entry.DestroyOwned(material); }
        }
    }
}
