#include "self_model/model_directory.h"
#include "self_model/model_format.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace ft::self_model;
void check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> void reject(F action,const char* expected=nullptr) {
    try { action(); }
    catch(const std::exception& e) {
        if(expected) check(std::string(e.what()).find(expected)!=std::string::npos,"Unexpected rejection reason");
        return;
    }
    throw std::runtime_error("Unsupported model was accepted");
}
std::vector<uint8_t> bytes(const std::string& path) {
    std::ifstream stream(path,std::ios::binary);
    check(bool(stream),"Missing test fixture");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
void checkGlb(const Model& model) {
    check(model.gltfMaterials && model.vertices.size()==3 && model.indices.size()==3 && model.parts.size()==1,
          "Valid skinned GLB geometry was not preserved");
    check(model.materials.size()==1 && std::abs(model.materials[0].gltf.roughness-.7f)<1e-6f,
          "GLB material settings changed");
    const auto hair=std::find_if(model.bones.begin(),model.bones.end(),[](const auto& bone){return bone.name=="Hair";});
    check(hair!=model.bones.end() && hair->dynamics==1 && model.colliders.size()==1,
          "GLB dynamics/collider extras were lost");
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"Pass fixture directory");const std::string root=argv[1];
        check(DirectoryModel{}.file=="model.glb","Directory entry default is not GLB");
        const auto scan=scanModelDirectory(root);
        check(scan.models.size()==4,"Scanner admitted an unsupported model or lost a valid GLB");
        for(const auto& entry:scan.models) {
            check(entry.id=="default" || entry.id=="named" || entry.id=="both" || entry.id=="empty",
                  "Unsupported format appeared in the catalog");
            checkGlb(readModelDirectory(root,entry.folder,entry.file));
            check(entry.file==(entry.id=="named"?"avatar.glb":"model.glb"),"Default GLB filename or explicit filename changed");
        }
        check(scan.diagnostics.find("Only .glb models are supported")!=std::string::npos,"Unsupported format has no diagnostic");
        check(scan.diagnostics.find("Missing resource: model.glb")!=std::string::npos,"Legacy-only directory silently fell back");
        auto model=readModelDirectory(root,"default");checkGlb(model);
        check(std::abs(model.lighting.shadowOffset-.12f)<1e-6f,"GLB optional-lighting default changed");
        model=readModelDirectory(root,"named","avatar.glb");
        check(std::abs(model.lighting.rim-.4f)<1e-6f,"GLB lighting override was lost");
        for(const auto* file:{"model.enstu3d","avatar.gltf","models.bin","model.GLB","model.glb.enstu3d"})
            reject([&]{readModelDirectory(root,"direct",file);},"Only .glb models are supported");
        reject([&]{readModelDirectory(root,"legacy_implicit");},"Missing selected model resource");
        reject([&]{readModelDirectory(root,"direct","legacy.glb");});
        reject([&]{readModelDirectory(root,"direct","json.glb");});
        reject([&]{readModelDirectory(root,"direct","../default/model.glb");},"Invalid model resource filename");
        checkGlb(decodeGlb(bytes(root+"/default/model.glb")));
        for(const auto* file:{"legacy.glb","json.glb","models.bin"})
            reject([&]{decodeGlb(bytes(root+"/direct/"+file));});
        ModelDirectory catalog;catalog.update(scan);
        {
            std::ofstream metadata(root+"/default/model.json");
            metadata<<"{\"id\":\"default\",\"name\":\"changed\",\"model\":\"model.enstu3d\"}";
            check(bool(metadata),"Cannot update model fixture metadata");
        }
        catalog.update(scanModelDirectory(root));
        for(const auto& entry:catalog.entries())
            check(entry.available==(entry.id!="default"),"Changing a selected GLB to an unsupported format did not invalidate its catalog entry");
        std::cout<<"PASS: GLB-only catalog, direct loading, content rejection and GLB feature preservation\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
