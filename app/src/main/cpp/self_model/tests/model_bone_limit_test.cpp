#include "self_model/model_format.h"
#include "self_model/model_pose.h"
#include "self_model/model_dynamics.h"
#include "third_party/nlohmann/json.hpp"
#include <cstring>
#include <iostream>

using namespace ft::self_model;
using Json=nlohmann::json;
static_assert(maxModelBones==1024,"The supported skeleton limit is 1024");
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void reject(F action,const char* reason=nullptr) {
    try { action(); }
    catch(const std::exception& e) {
        if(reason) check(std::string(e.what()).find(reason)!=std::string::npos,"Unexpected rejection reason");
        return;
    }
    throw std::runtime_error("Invalid skeleton was accepted");
}
template<class T> void write(std::vector<uint8_t>& bytes,size_t at,T value) {
    std::memcpy(bytes.data()+at,&value,sizeof(value));
}
struct Fixture {
    Json doc;
    std::vector<uint8_t> binary;
    template<class T> size_t attribute(const std::vector<T>& values,int component,const char* type,size_t count) {
        const auto offset=binary.size(),length=values.size()*sizeof(T);
        binary.resize(offset+length);
        std::memcpy(binary.data()+offset,values.data(),length);
        while(binary.size()%4) binary.push_back(0);
        auto& views=doc["bufferViews"]; auto& accessors=doc["accessors"];
        views.push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",length}});
        accessors.push_back({{"bufferView",views.size()-1},{"componentType",component},{"type",type},{"count",count}});
        return accessors.size()-1;
    }
    std::vector<uint8_t> pack() const {
        auto document=doc;
        document["buffers"]={{{"byteLength",binary.size()}}};
        auto json=document.dump(); while(json.size()%4) json+=' ';
        std::vector<uint8_t> result(28+json.size()+binary.size());
        write<uint32_t>(result,0,0x46546c67);write<uint32_t>(result,4,2);
        write<uint32_t>(result,8,uint32_t(result.size()));
        write<uint32_t>(result,12,uint32_t(json.size()));write<uint32_t>(result,16,0x4e4f534a);
        std::memcpy(result.data()+20,json.data(),json.size());
        write<uint32_t>(result,20+json.size(),uint32_t(binary.size()));
        write<uint32_t>(result,24+json.size(),0x004e4942);
        std::memcpy(result.data()+28+json.size(),binary.data(),binary.size());
        return result;
    }
};
Fixture fixture(size_t count) {
    Fixture f;
    f.doc={{"asset",{{"version","2.0"}}},{"scene",0},{"scenes",{{{"nodes",{count-1}}}}}};
    const auto positions=f.attribute<float>({0,0,0,1,0,0,0,1,0},5126,"VEC3",3);
    const auto normals=f.attribute<float>({0,0,1,0,0,1,0,0,1},5126,"VEC3",3);
    std::vector<uint16_t> joints;
    for(int i=0;i<3;++i) joints.insert(joints.end(),{uint16_t(count-1),uint16_t(count-2),0,0});
    const auto jointAttribute=f.attribute(joints,5123,"VEC4",3);
    const auto weights=f.attribute<float>({.75f,.25f,0,0,.75f,.25f,0,0,.75f,.25f,0,0},5126,"VEC4",3);
    std::vector<float> binds(count*16);
    for(size_t i=0;i<count;++i) {
        for(int k=0;k<4;++k) binds[i*16+k*5]=1;
        binds[i*16+13]=-float(i)*.001f;
    }
    const auto inverse=f.attribute(binds,5126,"MAT4",count);
    auto jointNodes=Json::array();
    // Leaf-first node storage forces the production decoder to remap every index.
    // The root also holds the skinned mesh, allowing exactly 1024 skin joints.
    for(size_t node=0;node<count;++node) {
        const auto logical=count-1-node;
        Json value={{"name","Joint_"+std::to_string(logical)}};
        if(node) value["children"]={node-1};
        if(logical) value["translation"]={0,.001,0};
        else { value["mesh"]=0;value["skin"]=0;value["extras"]["enstuHumanBone"]=0; }
        if(logical==count-1) value["extras"]={{"enstuDynamics",1},{"enstuTail",{0,.1,0}}};
        if(logical==count-2) value["extras"]["enstuColliders"]={{{"offset",{1,0,0}},{"radius",.01},{"mask",2}}};
        f.doc["nodes"].push_back(value);
        jointNodes.push_back(count-1-node);
    }
    f.doc["skins"]={{{"joints",jointNodes},{"inverseBindMatrices",inverse}}};
    f.doc["meshes"]={{{"primitives",{{{"attributes",{{"POSITION",positions},{"NORMAL",normals},
        {"JOINTS_0",jointAttribute},{"WEIGHTS_0",weights}}}}}}}};
    return f;
}
void skinning(const Model& model,size_t count) {
    check(model.bones.size()==count && model.vertices.size()==3,"Skeleton/geometry count changed");
    for(size_t i=0;i<count;++i) {
        check(model.bones[i].name=="Joint_"+std::to_string(i),"Topological remap failed");
        check(model.bones[i].parent==(i?int(i)-1:-1),"High parent index was truncated");
    }
    for(const auto& v:model.vertices)
        check(v.bones[0]==count-1 && v.bones[1]==count-2,"High weighted joint index was truncated");
    std::vector<std::optional<Matrix>> rest(count),now(count);
    const auto pose=retarget(model,rest,now);
    const auto matrices=skinMatrices(model,pose);
    for(const auto& v:model.vertices)
        check(glm::length(vec(skin(v,matrices).first)-vec(v.position))<1e-4f,"Rest skinning changed geometry");
    auto moved=pose; moved.back().position.x+=2;
    const auto translated=skinMatrices(model,moved);
    for(const auto& v:model.vertices)
        check(glm::length(vec(skin(v,translated).first)-vec(v.position)-glm::vec3(1.5f,0,0))<1e-4f,
              "Last joint does not drive its weighted vertices");
    Matrix identity;for(int i=0;i<4;++i) identity.m[i*5]=1;
    rest[0]=identity;
    const auto turn=glm::mat4_cast(glm::angleAxis(glm::half_pi<float>(),glm::vec3(0,0,1)));
    Matrix current;std::copy(glm::value_ptr(turn),glm::value_ptr(turn)+16,current.m.begin());now[0]=current;
    const auto rotated=skinMatrices(model,retarget(model,rest,now));
    for(const auto& v:model.vertices)
        check(glm::length(vec(skin(v,rotated).first)-glm::vec3(turn*glm::vec4(vec(v.position),1)))<1e-4f,
              "Root motion did not propagate through the full hierarchy");
}

void hairContacts() {
    Model model;model.bones.resize(1024);model.bones[0].human=0;
    model.bones[1022].position={.186f,0,0};
    model.bones[1023].dynamics=1;model.bones[1023].tail={.0998f,0,0};
    model.colliders.push_back({1022,-1,2,.115f,{0,0,0}});
    std::vector<std::optional<Matrix>> mapped(model.bones.size());
    for(bool capsule:{false,true}) {
        auto& collider=model.colliders[0];collider.localCapsule=capsule;
        if(capsule) {collider.offset={0,-.2f,0};collider.endOffset={0,.2f,0};}
        ModelDynamics dynamics;dynamics.prepare(model);
        for(int frame=0;frame<60;++frame) {
            auto pose=retarget(model,mapped,mapped);
            dynamics.update(model,pose,mapped,glm::mat4(1),{0,0,0},1.f/60);
            const auto origin=vec(pose.back().position),tail=origin+quat(pose.back().rotation)*vec(model.bones.back().tail);
            const auto a=vec(pose[1022].position)+vec(collider.offset);
            const auto segment=capsule?vec(collider.endOffset)-vec(collider.offset):glm::vec3(0);
            const auto closest=a+segment*std::clamp(glm::dot(tail-a,segment)/std::max(glm::dot(segment,segment),1e-12f),0.f,1.f);
            check(finite(tail) && std::abs(glm::length(tail-origin)-.0998f)<1e-5f,"Hair contact changed bone length");
            check(glm::length(tail-closest)>=collider.radius-1e-5f,"Collinear hair re-entered its sphere/capsule collider");
        }
    }
}

int main() {
    try {
        for(size_t count:{size_t(256),size_t(257),size_t(1024)}) {
            const auto model=decodeGlb(fixture(count).pack());skinning(model,count);
            check(model.colliders.size()==1 && model.colliders[0].bone==int(count)-2,"Collider index remapping failed");
            check(model.bones.back().dynamics==1,"High-index hair dynamics were lost");
            ModelDynamics dynamics;dynamics.prepare(model);
            std::vector<std::optional<Matrix>> mapped(count);
            for(int frame=0;frame<120;++frame) {
                auto pose=retarget(model,mapped,mapped);
                dynamics.update(model,pose,mapped,glm::mat4(1),{.1f*std::sin(frame*.1f),0,0},1.f/60);
                for(const auto& v:model.vertices) check(finite(vec(skin(v,skinMatrices(model,pose)).first)),"Non-finite high-index dynamics");
            }
        }
        reject([]{decodeGlb(fixture(1025).pack());},"1024 nodes");
        auto f=fixture(1024);f.doc["skins"][0]["joints"].push_back(0);
        reject([&]{decodeGlb(f.pack());},"1024 joints");
        f=fixture(1024);f.doc["skins"][0]["joints"]=Json::array();
        reject([&]{decodeGlb(f.pack());});
        f=fixture(1024);
        const auto jointView=f.doc["accessors"][2]["bufferView"].get<size_t>();
        write<uint16_t>(f.binary,f.doc["bufferViews"][jointView]["byteOffset"].get<size_t>(),1024);
        reject([&]{decodeGlb(f.pack());});
        f=fixture(1024);f.doc["nodes"][0]["children"]={1023};
        reject([&]{decodeGlb(f.pack());});
        hairContacts();
        std::cout<<"PASS: 256/257/1024 nodes, full hierarchy, joint 1023 skinning/dynamics, 1025 and invalid data rejected\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
