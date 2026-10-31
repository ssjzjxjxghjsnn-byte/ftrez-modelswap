#include "self_model/model_format.h"
#include "self_model/model_pose.h"
#include "self_model/bone_mapping.h"
#include "third_party/nlohmann/json.hpp"
#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb/stb_image.h"
#pragma GCC diagnostic pop
#include <memory>
#include <numeric>
#include <set>

namespace ft::self_model {
namespace {
void need(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
using Json=nlohmann::json;
std::optional<Json> collisionMetadata(const cgltf_extras& extras,const char* key) {
    if(!extras.data) return std::nullopt;
    const auto object=Json::parse(extras.data);
    if(!object.is_object() || !object.contains(key)) return std::nullopt;
    auto value=object.at(key);
    if(value.is_string()) value=Json::parse(value.get<std::string>());
    need(value.is_object(),"Collision hook metadata must be an object or JSON object string");
    return value;
}
void collisionKeys(const Json& value,std::initializer_list<const char*> allowed) {
    for(auto it=value.begin();it!=value.end();++it)
        need(std::any_of(allowed.begin(),allowed.end(),[&](const char* key){return it.key()==key;}),"Unknown collision hook metadata field");
}
int collisionInteger(const Json& value,int low,int high,const char* error) {
    need(value.is_number_integer() && value>=low && value<=high,error); return value.get<int>();
}
void parseCollisionHook(const Json& value,CollisionHook& hook) {
    need(!hook.specified,"Duplicate ftRezCollisionHook declaration");
    collisionKeys(value,{"version","enabled","iterations","paddingRatio","maxDisplacementRatio"});
    need(value.contains("version"),"Missing ftRezCollisionHook version");
    collisionInteger(value.at("version"),1,1,"Unsupported ftRezCollisionHook version");
    if(value.contains("enabled")) {
        need(value.at("enabled").is_boolean(),"ftRezCollisionHook enabled must be boolean"); hook.enabled=value.at("enabled").get<bool>();
    }
    if(value.contains("iterations")) hook.iterations=uint8_t(collisionInteger(value.at("iterations"),1,16,"Invalid ftRezCollisionHook iterations"));
    auto ratio=[&](const char* name,float fallback,float maximum) {
        if(!value.contains(name)) return fallback;
        need(value.at(name).is_number(),"Collision hook ratio must be numeric");
        const float result=value.at(name).get<float>();
        need(std::isfinite(result) && result>=.00001f && result<=maximum,"Invalid collision hook ratio"); return result;
    };
    hook.paddingRatio=ratio("paddingRatio",hook.paddingRatio,.02f);
    hook.maxDisplacementRatio=ratio("maxDisplacementRatio",hook.maxDisplacementRatio,.25f);
    need(hook.paddingRatio<=hook.maxDisplacementRatio,"Collision padding exceeds maximum displacement");
    hook.specified=true;
}
void parseCollisionRole(const Json& value,Part& part) {
    collisionKeys(value,{"role","mask"});
    need(value.contains("role") && value.at("role").is_string(),"Missing collision hook role");
    const auto role=value.at("role").get<std::string>();
    need(role=="ignore" || role=="collider" || role=="deform","Invalid collision hook role");
    part.collisionRole=role=="deform"?CollisionRole::Deform:role=="collider"?CollisionRole::Collider:CollisionRole::Ignore;
    part.collisionMask=value.contains("mask")?uint16_t(collisionInteger(value.at("mask"),1,65535,"Invalid collision hook mask")):1;
}
glm::mat4 checked(glm::mat4 m) {
    for(int i=0;i<16;++i) need(std::isfinite(glm::value_ptr(m)[i]),"GLB non-finite transform");
    need(std::abs(glm::determinant(m))>1e-10f,"GLB singular transform");
    return m;
}
const cgltf_accessor* attribute(const cgltf_primitive& p,cgltf_attribute_type type,int index=0) {
    for(size_t i=0;i<p.attributes_count;++i) if(p.attributes[i].type==type && p.attributes[i].index==index) return p.attributes[i].data;
    return nullptr;
}
void readValue(const cgltf_accessor* a,size_t index,float* out,size_t count) {
    need(a && index<a->count && !a->is_sparse && cgltf_num_components(a->type)==count && cgltf_accessor_read_float(a,index,out,count),"GLB invalid or sparse accessor");
    for(size_t i=0;i<count;++i) need(std::isfinite(out[i]),"GLB non-finite vertex data");
}
int humanoid(const std::string& name) {
    auto key=canonical(name);
    static const std::map<std::string,std::string> aliases{
        {"leftupleg","lthigh"},{"rightupleg","rthigh"},{"leftleg","lcalf"},{"rightleg","rcalf"},
        {"leftarm","lupperarm"},{"rightarm","rupperarm"},{"leftforearm","lforearm"},{"rightforearm","rforearm"},
        {"thighl","lthigh"},{"thighr","rthigh"},{"shinl","lcalf"},{"shinr","rcalf"},
        {"footl","lfoot"},{"footr","rfoot"},{"toel","ltoe0"},{"toer","rtoe0"},
        {"shoulderl","lclavicle"},{"shoulderr","rclavicle"},{"upperarml","lupperarm"},{"upperarmr","rupperarm"},
        {"forearml","lforearm"},{"forearmr","rforearm"},{"handl","lhand"},{"handr","rhand"}
    };
    auto alias=aliases.find(key);if(alias!=aliases.end()) key=alias->second;
    for(int human=0;human<55;++human) if(key==humanBoneName(human)) return human;
    const char* fingers[]={"thumb","index","middle","ring","pinky"};
    for(int side=0;side<2;++side) for(int finger=0;finger<5;++finger) for(int segment=1;segment<=3;++segment)
        if(key==std::string(side?"righthand":"lefthand")+fingers[finger]+std::to_string(segment))
            return 24+side*15+finger*3+segment-1;
    return -1;
}
Matrix stored(const glm::mat4& m) { Matrix result;std::copy_n(glm::value_ptr(m),16,result.m.begin());return result; }
ModelImage gltfImage(const cgltf_texture_view& view,bool linear,bool hasUV) {
    ModelImage result; result.sampling.linear=linear;
    if(!view.texture) return result;
    need(hasUV && view.texcoord==0 && !view.has_transform,"GLB textures require TEXCOORD_0 without texture transforms");
    const auto* image=view.texture->image;
    need(image && !image->uri && image->buffer_view && image->mime_type &&
        (std::string(image->mime_type)=="image/png" || std::string(image->mime_type)=="image/jpeg"),
        "GLB textures must be embedded PNG or JPEG");
    auto* bytes=cgltf_buffer_view_data(image->buffer_view); auto size=image->buffer_view->size;
    need(bytes && size>0 && size<=16*1024*1024,"GLB texture size limit");
    int w=0,h=0,c=0;
    need(stbi_info_from_memory(bytes,int(size),&w,&h,&c) && w>0 && h>0 && w<=4096 && h<=4096,
        "GLB invalid texture or dimensions exceed 4096");
    result.png.assign(bytes,bytes+size);
    if(const auto* sampler=view.texture->sampler) {
        auto wrap=[](int value) { return value==33071?1:value==33648?2:0; };
        result.sampling.wrapU=wrap(sampler->wrap_s); result.sampling.wrapV=wrap(sampler->wrap_t);
        const int min=sampler->min_filter,mag=sampler->mag_filter;
        result.sampling.mipmaps=min!=9728 && min!=9729;
        result.sampling.filter=(min==9728 || min==9984 || (min==0 && mag==9728))?0:
                               (min==9986 || min==9987)?2:1;
    }
    return result;
}
void embeddedRenderer(const cgltf_data& data,Model& model) {
    std::optional<Json> descriptor;
    auto collect=[&](const cgltf_extras& extras) {
        if(auto value=collisionMetadata(extras,"ftRezRenderer")) {
            need(!descriptor,"Duplicate ftRezRenderer declaration"); descriptor=std::move(value);
        }
    };
    collect(data.extras);
    for(size_t i=0;i<data.nodes_count;++i) collect(data.nodes[i].extras);
    if(!descriptor) return;
    model.rendererEmbedded=true;
    try {
        const auto& d=*descriptor;
        collisionKeys(d,{"version","bufferView","shader","material","colorSpace"});
        need(d.contains("version") && d["version"].is_number_integer() && d["version"]==1,"Unsupported ftRezRenderer version");
        need(d.value("colorSpace",std::string{})=="Linear","Embedded renderer must be built for Linear color space");
        const auto index=collisionInteger(d.at("bufferView"),0,int(data.buffer_views_count)-1,"Invalid renderer bufferView");
        auto path=[&](const char* key,bool required) {
            if(!d.contains(key) && !required) return std::string{};
            need(d.contains(key) && d[key].is_string(),"Invalid renderer asset path");
            auto value=d[key].get<std::string>();
            need(!value.empty() && value.size()<=512 && value.find('\0')==std::string::npos,"Invalid renderer asset path");
            return value;
        };
        auto shader=path("shader",true),material=path("material",false);
        const auto& view=data.buffer_views[index]; auto* bytes=cgltf_buffer_view_data(&view);
        need(bytes && view.size>=16 && view.size<=16*1024*1024,"Embedded renderer exceeds 16 MiB or is empty");
        need(!std::memcmp(bytes,"UnityFS\0",8),"Embedded renderer must contain an Android UnityFS AssetBundle");
        model.rendererBytes.assign(bytes,bytes+view.size);
        model.rendererShader=std::move(shader); model.rendererMaterial=std::move(material);
    } catch(const std::exception& error) { model.rendererError=error.what(); }
}
}
Model decodeGlb(const std::vector<uint8_t>& bytes) {
    need(bytes.size()>=20 && bytes.size()<=maxModelBytes,"Invalid GLB size");
    cgltf_options options{};cgltf_data* raw=nullptr;
    need(cgltf_parse(&options,bytes.data(),bytes.size(),&raw)==cgltf_result_success,"Cannot parse GLB");
    std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,cgltf_free);auto& d=*data;
    need(d.file_type==cgltf_file_type_glb,"Expected binary glTF 2.0");
    for(size_t i=0;i<d.extensions_required_count;++i) {
        const std::string extension=d.extensions_required[i];
        need(extension=="KHR_materials_unlit" || extension=="KHR_materials_emissive_strength",
             "GLB required extension unsupported");
    }
    need(d.buffers_count==1 && !d.buffers[0].uri && d.bin,"GLB must embed its buffer");
    need(cgltf_load_buffers(&options,&d,nullptr)==cgltf_result_success,"GLB missing embedded buffer");
    need(cgltf_validate(&d)==cgltf_result_success,"Invalid GLB accessors or hierarchy");
    need(d.nodes_count>0 && d.nodes_count<=256 && d.skins_count==1,"GLB requires one armature and at most 256 nodes");
    auto& skin=d.skins[0];need(skin.joints_count>0 && skin.joints_count<=256,"GLB missing joints");

    std::vector<int> order(d.nodes_count),depth(d.nodes_count);
    std::iota(order.begin(),order.end(),0);
    for(size_t i=0;i<d.nodes_count;++i) {
        auto* p=d.nodes[i].parent;size_t n=0;
        while(p) {need(++n<=d.nodes_count,"GLB cyclic hierarchy");p=p->parent;} depth[i]=int(n);
    }
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return depth[a]<depth[b];});
    std::vector<int> remap(d.nodes_count);for(size_t i=0;i<order.size();++i) remap[order[i]]=int(i);
    std::vector<glm::mat4> worlds(d.nodes_count),binds(d.nodes_count);
    for(size_t i=0;i<d.nodes_count;++i) {cgltf_node_transform_world(&d.nodes[i],glm::value_ptr(worlds[i]));binds[i]=checked(worlds[i]);}
    std::vector<cgltf_node*> meshes;
    for(size_t i=0;i<d.nodes_count;++i) if(d.nodes[i].mesh) {
        need(d.nodes[i].skin==&skin,"Every GLB mesh must use the same armature");
        need(!d.nodes[i].has_mesh_gpu_instancing,"GLB instancing unsupported");meshes.push_back(&d.nodes[i]);
    }
    need(!meshes.empty(),"GLB has no skinned mesh");

    for(size_t j=0;j<skin.joints_count;++j) {
        glm::mat4 inverse(1);if(skin.inverse_bind_matrices) readValue(skin.inverse_bind_matrices,j,glm::value_ptr(inverse),16);
        const auto world=checked(worlds[meshes[0]-d.nodes]*glm::inverse(checked(inverse)));
        for(auto* mesh:meshes) {
            auto candidate=worlds[mesh-d.nodes]*glm::inverse(inverse);
            for(int k=0;k<16;++k) need(std::abs(glm::value_ptr(candidate)[k]-glm::value_ptr(world)[k])<.001f,"GLB meshes have incompatible bind spaces; apply transforms before export");
        }
        binds[skin.joints[j]-d.nodes]=world;
    }
    glm::mat4 flip(1);flip[2][2]=-1;Model model;
    model.gltfMaterials=true;
    embeddedRenderer(d,model);
    if(auto value=collisionMetadata(d.extras,"ftRezCollisionHook")) parseCollisionHook(*value,model.collisionHook);


    for(size_t i=0;i<d.nodes_count;++i)
        if(auto value=collisionMetadata(d.nodes[i].extras,"ftRezCollisionHook")) parseCollisionHook(*value,model.collisionHook);
    bool hasCollisionRoles=false;
    for(int index:order) {
        auto& node=d.nodes[index];Bone bone;bone.name=node.name?node.name:"node_"+std::to_string(index);
        bone.parent=node.parent?int16_t(remap[node.parent-d.nodes]):-1;bone.flags=1;
        auto world=checked(flip*binds[index]*flip);bone.position=vec(glm::vec3(world[3]));bone.rotation=quat(rotation(world));
        bone.scale={glm::length(glm::vec3(world[0])),glm::length(glm::vec3(world[1])),glm::length(glm::vec3(world[2]))};
        for(int a=0;a<3;++a) for(int b=a+1;b<3;++b)
            need(std::abs(glm::dot(glm::normalize(glm::vec3(world[a])),glm::normalize(glm::vec3(world[b]))))<.001f,"GLB sheared bone transforms unsupported; apply transforms before export");
        bone.inverseBind=stored(glm::inverse(world));
        bone.human=int16_t(humanoid(bone.name));
        if(node.extras.data) {
            auto extras=Json::parse(node.extras.data);
            if(extras.is_object() && extras.contains("enstuHumanBone")) {
                int human=extras.at("enstuHumanBone").get<int>();need(human>=-1 && human<55,"Invalid GLB humanoid bone ID");bone.human=int16_t(human);
            }
            if(extras.is_object() && extras.contains("enstuDynamics")) {
                int category=extras.at("enstuDynamics").get<int>();
                need(category>=0 && category<=3 && (category==0 || bone.human<0),"Invalid GLB dynamics category");
                bone.dynamics=uint16_t(category);
                if(category) {
                auto tail=extras.at("enstuTail").get<std::vector<float>>();
                need(tail.size()==3,"Invalid GLB dynamics tail");

                glm::vec3 delta(tail[0],tail[1],-tail[2]);
                need(finite(delta) && glm::length(delta)>1e-6f,"Invalid GLB dynamics length");
                bone.tail=vec(glm::inverse(quat(bone.rotation))*delta);
                if(extras.value("enstuSkirt",false)) bone.flags|=128;
                }
            }
            if(extras.is_object() && extras.contains("enstuColliders")) {
                auto colliders=extras.at("enstuColliders");
                if(colliders.is_string()) colliders=Json::parse(colliders.get<std::string>());
                need(colliders.is_array() && colliders.size()<=8,"Invalid GLB colliders");
                for(const auto& c:colliders) {
                    auto offset=c.at("offset").get<std::vector<float>>();
                    float radius=c.at("radius").get<float>();
                    int mask=c.at("mask").get<int>();
                    need(offset.size()==3 && std::isfinite(radius) && radius>0 && mask>0 && mask<=14,"Invalid GLB collider");
                    glm::vec3 delta(offset[0],offset[1],-offset[2]);
                    need(finite(delta),"Invalid GLB collider offset");
                    Collider collider{int16_t(model.bones.size()),-1,uint16_t(mask),radius,vec(glm::inverse(quat(bone.rotation))*delta)};
                    if(c.contains("endOffset")) {
                        const auto end=c.at("endOffset").get<std::vector<float>>();
                        need(end.size()==3,"Invalid GLB capsule endpoint");
                        const glm::vec3 endDelta(end[0],end[1],-end[2]);
                        need(finite(endDelta) && glm::length(endDelta-delta)>1e-6f,"Invalid GLB capsule segment");
                        collider.endOffset=vec(glm::inverse(quat(bone.rotation))*endDelta);
                        collider.localCapsule=true;
                    }
                    model.colliders.push_back(collider);
                }
            }
        }
        model.bones.push_back(bone);
    }


    for(size_t i=0;i<order.size();++i) {
        const auto& node=d.nodes[order[i]];
        if(!node.extras.data) continue;
        const auto extras=Json::parse(node.extras.data);
        if(!extras.is_object() || !extras.contains("enstuAimHuman")) continue;
        const int target=extras.at("enstuAimHuman").get<int>();
        need(target>=0 && target<55,"Invalid GLB aim human ID");
        int found=-1;
        for(size_t j=0;j<model.bones.size();++j) if(model.bones[j].human==target) {
            need(found<0,"Ambiguous GLB aim joint");found=int(j);
        }
        need(found>=0 && found!=int(i),"Missing or self-referential GLB aim joint");
        int parent=model.bones[found].parent;
        while(parent>=0 && parent!=int(i)) parent=model.bones[parent].parent;
        need(parent==int(i),"GLB aim joint must be a descendant");
        auto& bone=model.bones[i];
        auto delta=vec(model.bones[found].position)-vec(bone.position);
        need(glm::length(delta)>1e-6f,"GLB aim chain has zero length");
        bone.aim=int16_t(found);bone.flags|=4;
        bone.tail=vec(glm::inverse(quat(bone.rotation))*delta);
    }
    for(auto* node:meshes) for(size_t primitive=0;primitive<node->mesh->primitives_count;++primitive) {
        const auto& p=node->mesh->primitives[primitive];
        need(p.type==cgltf_primitive_type_triangles && !p.has_draco_mesh_compression && p.targets_count==0,"GLB requires uncompressed triangles without shape keys");
        need(model.parts.size()<32,"GLB exceeds 32 mesh parts");
        auto pos=attribute(p,cgltf_attribute_type_position),normal=attribute(p,cgltf_attribute_type_normal);
        auto joints=attribute(p,cgltf_attribute_type_joints),weights=attribute(p,cgltf_attribute_type_weights);
        auto uv=attribute(p,cgltf_attribute_type_texcoord);
        need(pos && normal && joints && weights && pos->count>0 && pos->count<=65535,"GLB needs positions, normals and four joint weights; max 65535 vertices per part");
        need(!attribute(p,cgltf_attribute_type_joints,1) && !attribute(p,cgltf_attribute_type_weights,1),"GLB supports at most four bone influences; limit weights before export");
        need(model.vertices.size()+pos->count<=1000000,"GLB vertex limit exceeded");
        auto meshWorld=checked(worlds[node-d.nodes]);auto transform=flip*meshWorld;
        auto normals=glm::transpose(glm::inverse(glm::mat3(transform)));
        Part part;part.name=node->mesh->name?node->mesh->name:"mesh";part.name+="_"+std::to_string(primitive);
        part.firstVertex=uint32_t(model.vertices.size());part.vertexCount=uint32_t(pos->count);
        part.firstIndex=uint32_t(model.indices.size());part.material=uint16_t(model.materials.size());
        if(p.material) if(auto value=collisionMetadata(p.material->extras,"ftRezCollision")) {
            parseCollisionRole(*value,part); hasCollisionRoles=true;
        }

        if(auto value=collisionMetadata(p.extras,"ftRezCollision")) {
            parseCollisionRole(*value,part); hasCollisionRoles=true;
        }
        auto visibility=[&](const cgltf_extras& extras) {
            if(!extras.data) return;
            const auto value=Json::parse(extras.data);
            if(!value.is_object() || !value.contains("ftRezRenderVisible")) return;
            need(value["ftRezRenderVisible"].is_boolean(),"ftRezRenderVisible must be boolean");
            part.renderVisible=value["ftRezRenderVisible"].get<bool>();
        };
        if(p.material) visibility(p.material->extras);
        visibility(p.extras);
        need(part.renderVisible || part.collisionRole==CollisionRole::Collider,
             "Hidden mesh must declare a collider role");
        Material material;material.name=p.material && p.material->name?p.material->name:part.name;material.color={1,1,1,1};
        material.gltf.specified=true;
        if(p.material) {
            const auto& mat=*p.material;
            if(mat.extras.data) {
                const auto extras=Json::parse(mat.extras.data);
                if(extras.is_object() && extras.contains("ftRezShaderFloats")) {
                    auto values=extras.at("ftRezShaderFloats");
                    if(values.is_string()) values=Json::parse(values.get<std::string>());
                    need(values.is_object() && values.size()<=32,"Invalid shader float metadata");
                    for(auto it=values.begin();it!=values.end();++it) {
                        const auto& key=it.key();
                        need(key.size()>1 && key.size()<=64 && key[0]=='_' &&
                            key.find_first_not_of("_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")==std::string::npos,
                            "Invalid shader property name");
                        need(it.value().is_number(),"Shader property must be numeric");
                        const float value=it.value().get<float>();
                        need(std::isfinite(value) && std::abs(value)<=10000,"Invalid shader property value");
                        material.shaderFloats.emplace_back(key,value);
                    }
                }
            }
            if(mat.double_sided) material.flags|=materialDoubleSided;
            auto& render=material.gltf;
            render.alphaMode=mat.alpha_mode==cgltf_alpha_mode_mask?1:mat.alpha_mode==cgltf_alpha_mode_blend?2:0;
            render.alphaCutoff=mat.alpha_cutoff;
            render.unlit=mat.unlit;
            std::copy_n(mat.emissive_factor,3,render.emission.begin());
            if(mat.has_emissive_strength) for(auto& c:render.emission) c*=mat.emissive_strength.emissive_strength;
            render.normalScale=mat.normal_texture.scale;
            render.occlusionStrength=mat.occlusion_texture.scale;
            auto image=[&](const cgltf_texture_view& view,bool linear) {
                if(!view.texture) return -1;
                auto loaded=gltfImage(view,linear,uv!=nullptr);
                for(size_t i=0;i<model.images.size();++i) if(model.images[i].sampling==loaded.sampling && model.images[i].png==loaded.png) return int(i);
                model.images.push_back(std::move(loaded)); return int(model.images.size()-1);
            };
            render.normal=image(mat.normal_texture,true);
            render.occlusion=image(mat.occlusion_texture,true);
            render.emissive=image(mat.emissive_texture,false);
            if(mat.alpha_mode==cgltf_alpha_mode_blend) part.flags|=2;
            if(mat.has_pbr_metallic_roughness) {
                const auto& pbr=mat.pbr_metallic_roughness;
                std::copy_n(pbr.base_color_factor,4,material.color.begin());
                render.metallic=pbr.metallic_factor; render.roughness=pbr.roughness_factor;
                render.metallicRoughness=image(pbr.metallic_roughness_texture,true);
                auto base=gltfImage(pbr.base_color_texture,false,uv!=nullptr);
                material.png=std::move(base.png); material.sampling=base.sampling;
            }
            for(auto value:{render.metallic,render.roughness,render.occlusionStrength,render.alphaCutoff})
                need(std::isfinite(value) && value>=0 && value<=1,"Invalid GLB material factor");
            need(std::isfinite(render.normalScale) && render.normalScale>=0 && render.normalScale<=10000,"Invalid GLB normal scale");
            for(auto value:render.emission) need(std::isfinite(value) && value>=0 && value<=10000,"Invalid GLB emissive factor");
        }
        for(float c:material.color) need(std::isfinite(c) && c>=0 && c<=1,"Invalid GLB base color");
        model.materials.push_back(std::move(material));
        for(size_t v=0;v<pos->count;++v) {
            float point[3],n[3],tex[2]{},weight[4],joint[4];
            readValue(pos,v,point,3);readValue(normal,v,n,3);readValue(weights,v,weight,4);readValue(joints,v,joint,4);
            if(uv) readValue(uv,v,tex,2);
            Vertex out;auto pointWorld=glm::vec3(transform*glm::vec4(point[0],point[1],point[2],1));
            auto norm=normals*glm::vec3(n[0],n[1],n[2]);need(finite(pointWorld) && finite(norm) && glm::length(norm)>1e-8f,"GLB invalid position/normal");
            out.position=vec(pointWorld);out.normal=vec(glm::normalize(norm));out.uv={tex[0],1-tex[1]};float sum=0;
            for(size_t k=0;k<4;++k) {
                need(weight[k]>=0 && joint[k]>=0 && joint[k]<skin.joints_count && std::floor(joint[k])==joint[k],"GLB invalid skin weight or joint index");
                out.bones[k]=uint16_t(remap[skin.joints[size_t(joint[k])]-d.nodes]);out.weights[k]=weight[k];sum+=weight[k];
            }
            need(std::isfinite(sum) && sum>1e-8f,"GLB zero skin weights");for(auto& w:out.weights) w/=sum;
            model.vertices.push_back(out);
        }
        const size_t count=p.indices?p.indices->count:pos->count;
        need(count>0 && count%3==0 && model.indices.size()+count<=3000000,"GLB invalid triangle count");
        if(p.indices) need(!p.indices->is_sparse && p.indices->type==cgltf_type_scalar &&
            (p.indices->component_type==cgltf_component_type_r_8u || p.indices->component_type==cgltf_component_type_r_16u || p.indices->component_type==cgltf_component_type_r_32u),"GLB invalid index accessor");
        for(size_t i=0;i<count;i+=3) {
            uint32_t indices[3];for(size_t k=0;k<3;++k) {
                size_t index=p.indices?cgltf_accessor_read_index(p.indices,i+k):i+k;
                need(index<pos->count,"GLB index outside mesh");indices[k]=uint32_t(index);
            }
            if(glm::determinant(glm::mat3(transform))<0) std::swap(indices[1],indices[2]);
            model.indices.insert(model.indices.end(),indices,indices+3);
        }
        part.indexCount=uint32_t(count);model.parts.push_back(part);
    }
    need(!model.vertices.empty(),"GLB contains no geometry");
    need(!hasCollisionRoles || model.collisionHook.specified,"ftRezCollision requires ftRezCollisionHook version 1");
    if(model.collisionHook.specified && model.collisionHook.enabled) {
        size_t deformVertices=0,deformTriangles=0,bodyTriangles=0; uint16_t colliderMasks=0;
        for(const auto& part:model.parts) {
            if(part.collisionRole==CollisionRole::Deform) { deformVertices+=part.vertexCount; deformTriangles+=part.indexCount/3; }
            if(part.collisionRole==CollisionRole::Collider) { bodyTriangles+=part.indexCount/3; colliderMasks|=part.collisionMask; }
        }
        need(deformVertices && bodyTriangles,"Enabled collision hook requires collider and deform parts");
        need(deformVertices<=8192 && deformTriangles<=16384 && bodyTriangles<=60000,"Collision hook geometry limit exceeded");
        for(const auto& part:model.parts) if(part.collisionRole==CollisionRole::Deform)
            need((part.collisionMask&colliderMasks)!=0,"Collision hook deform mask has no matching collider");
    }
    return model;
}
}
