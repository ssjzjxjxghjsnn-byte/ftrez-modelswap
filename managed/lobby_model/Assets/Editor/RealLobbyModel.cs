using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

// Only a GPU fixture: original bind-pose geometry/materials, no animation import.
static class RealLobbyModel
{
    [Serializable] public sealed class Scalar { public string name; public float value; }
    [Serializable] public sealed class Part
    {
        public string name, file, image;
        public float[] color;
        public float metallic, roughness, cull;
        public Scalar[] floats;
    }
    [Serializable] public sealed class Document { public string source, sha256; public Part[] parts; }
    public static bool Enabled => File.Exists("ValidationModel/enabled.txt");
    public static Document Load() => JsonUtility.FromJson<Document>(File.ReadAllText("ValidationModel/model.json"));

    public static Renderer[] Create(Document doc, Transform root, string shaderName, List<Object> owned)
    {
        var shader = Shader.Find(shaderName);
        if (!shader || !shader.isSupported) throw new InvalidOperationException("Real model shader unavailable: " + shaderName);
        var textures = new Dictionary<string, Texture2D>();
        Texture2D Texture(string file, bool linear = false)
        {
            if (textures.TryGetValue(file, out var saved)) return saved;
            var texture = new Texture2D(2, 2, TextureFormat.RGBA32, true, linear); owned.Add(texture);
            if (!texture.LoadImage(File.ReadAllBytes("ValidationModel/" + file))) throw new InvalidDataException(file);
            texture.wrapMode = TextureWrapMode.Repeat; textures[file] = texture; return texture;
        }
        var renderers = new List<Renderer>();
        var bounds = new Bounds(); bool first = true;
        foreach (var part in doc.parts)
        {
            var mesh = new Mesh { name = part.name, indexFormat = IndexFormat.UInt32 }; owned.Add(mesh);
            using (var input = new BinaryReader(File.OpenRead("ValidationModel/" + part.file)))
            {
                int count = input.ReadInt32(), indexCount = input.ReadInt32();
                var vertices = new Vector3[count]; var normals = new Vector3[count]; var uv = new Vector2[count];
                for (int i = 0; i < count; ++i)
                {
                    vertices[i] = new Vector3(input.ReadSingle(), input.ReadSingle(), input.ReadSingle());
                    normals[i] = new Vector3(input.ReadSingle(), input.ReadSingle(), input.ReadSingle());
                    uv[i] = new Vector2(input.ReadSingle(), input.ReadSingle());
                }
                var indices = new int[indexCount]; for (int i = 0; i < indices.Length; ++i) indices[i] = input.ReadInt32();
                mesh.vertices = vertices; mesh.normals = normals; mesh.uv = uv; mesh.triangles = indices; mesh.RecalculateBounds();
            }
            if (first) { bounds = mesh.bounds; first = false; } else bounds.Encapsulate(mesh.bounds);
            var material = new Material(shader) { name = part.name }; owned.Add(material);
            material.SetColor("_BaseColor", new Color(part.color[0], part.color[1], part.color[2], part.color[3]));
            if (part.image.Length != 0) material.SetTexture("_BaseMap", Texture(part.image));
            foreach (var scalar in part.floats) if (material.HasProperty(scalar.name)) material.SetFloat(scalar.name, scalar.value);
            if (material.HasProperty("_Cull")) material.SetFloat("_Cull", part.cull);
            if (material.HasProperty("_Metallic")) material.SetFloat("_Metallic", part.metallic);
            if (material.HasProperty("_Roughness")) material.SetFloat("_Roughness", part.roughness);
            if (material.HasProperty("_SourceMasks")) material.SetTexture("_SourceMasks", Texture("source-masks.png", true));
            if (material.HasProperty("_BlenderPreview")) material.SetFloat("_BlenderPreview", 0);
            var go = new GameObject(part.name); go.layer = 8; go.transform.SetParent(root, false);
            go.AddComponent<MeshFilter>().sharedMesh = mesh;
            var renderer = go.AddComponent<MeshRenderer>(); renderer.sharedMaterial = material; renderers.Add(renderer);
        }
        float scale = 2.4f / bounds.size.y;
        root.localScale = Vector3.one * scale;
        root.position = -bounds.center * scale;
        return renderers.ToArray();
    }
}
