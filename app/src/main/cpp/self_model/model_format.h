#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace ft::self_model {
// Includes non-joint glTF hierarchy/mesh nodes retained by the pose evaluator.
constexpr size_t maxModelBones=1024;
struct Vec3 { float x=0,y=0,z=0; };
struct Quat { float x=0,y=0,z=0,w=1; };
struct Matrix { std::array<float,16> m{}; };
struct Bone {
    std::string name;
    int16_t parent=-1, human=-1, aim=-1, fingerTarget=-1;
    uint16_t flags=0, dynamics=0;
    Vec3 position;
    Vec3 scale{1,1,1};
    Quat rotation;
    Matrix inverseBind;
    Vec3 tail, pole;
};
struct Collider {
    int16_t bone=-1, end=-1;
    uint16_t mask=0;
    float radius=0;
    Vec3 offset;

    Vec3 endOffset{};
    bool localCapsule=false;
};
struct Vertex {
    Vec3 position, normal;
    std::array<float,2> uv;
    std::array<uint16_t,4> bones;
    std::array<float,4> weights;
};
struct TextureSampling {
    int wrapU=0,wrapV=0,filter=1; // Unity: Repeat/Clamp/Mirror; Point/Bilinear/Trilinear
    bool mipmaps=true,linear=false;
    bool operator==(const TextureSampling& b) const {
        return wrapU==b.wrapU && wrapV==b.wrapV && filter==b.filter && mipmaps==b.mipmaps && linear==b.linear;
    }
};
struct ModelImage { std::vector<uint8_t> png; TextureSampling sampling; };
struct GltfMaterial {
    bool specified=false,unlit=false;
    float metallic=1,roughness=1,normalScale=1,occlusionStrength=1,alphaCutoff=.5f;
    std::array<float,3> emission{}; // glTF factors are already Linear
    int alphaMode=0; // OPAQUE=0, MASK=1, BLEND=2
    int metallicRoughness=-1,normal=-1,occlusion=-1,emissive=-1; // Model::images
};
struct Material {
    std::string name;
    std::array<float,4> color;
    std::vector<uint8_t> png;
    uint32_t flags=0, outlineRGB=0;

    std::vector<std::pair<std::string,float>> shaderFloats;
    TextureSampling sampling;
    GltfMaterial gltf;
};

constexpr uint32_t materialDoubleSided = 0x10000;
struct Lighting { float rim=0, shadowOffset=.12f, shadowRamp=.04f; };


struct CollisionHook {
    bool specified=false,enabled=true;
    uint8_t iterations=8;
    float paddingRatio=.002f,maxDisplacementRatio=.12f;
};
enum class CollisionRole : uint8_t { Ignore,Collider,Deform };
struct Part {
    std::string name;
    uint16_t material=0, flags=0;
    uint32_t firstVertex=0, vertexCount=0, firstIndex=0, indexCount=0;
    CollisionRole collisionRole=CollisionRole::Ignore;
    uint16_t collisionMask=1;
    bool renderVisible=true;
};
struct Model {
    Lighting lighting;
    std::vector<uint8_t> rendererBytes;
    std::string rendererShader,rendererError;
    std::vector<Bone> bones;
    std::vector<Collider> colliders;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Material> materials;

    std::vector<int32_t> textureSources;
    std::vector<Part> parts;
    CollisionHook collisionHook;
    std::string rendererMaterial;
    bool gltfMaterials=false,rendererEmbedded=false;
    std::vector<ModelImage> images;
};
inline size_t modelTextureCount(const Model& model) { return model.materials.size()+model.images.size(); }
inline const std::vector<uint8_t>& modelTextureBytes(const Model& model,size_t i) {
    return i<model.materials.size()?model.materials.at(i).png:model.images.at(i-model.materials.size()).png;
}
inline const TextureSampling& modelTextureSampling(const Model& model,size_t i) {
    return i<model.materials.size()?model.materials.at(i).sampling:model.images.at(i-model.materials.size()).sampling;
}
constexpr size_t maxModelBytes=64*1024*1024;

std::string modelFolderName(const std::string& label);
Model readModelDirectory(const std::string& root,const std::string& folder,const std::string& file="model.glb");
void readModelRenderer(Model& model,const std::string& folder,const std::string& bundle,const std::string& shader,const std::string& error,const std::string& material={});
Model decodeGlb(const std::vector<uint8_t>& bytes);
}
