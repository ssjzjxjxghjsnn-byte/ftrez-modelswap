#include "self_model/model_format.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ft::self_model {
namespace {
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
std::vector<uint8_t> read(std::ifstream& f,size_t off,size_t size) {
    std::vector<uint8_t> b(size); f.seekg(off); f.read(reinterpret_cast<char*>(b.data()),size);
    require(bool(f),"Cannot read model resource"); return b;
}
}
std::string modelFolderName(const std::string& label) {
    std::string result=label;
    for(auto& c:result)
        if(c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|' ||
           static_cast<unsigned char>(c)<32) c='-';
    while(!result.empty() && (result.back()=='.' || result.back()==' ')) result.pop_back();
    require(!result.empty() && result!="." && result!="..","Invalid model folder name");
    return result;
}
Model readModelDirectory(const std::string& root,const std::string& folder,const std::string& file) {
    require(file==modelFolderName(file),"Invalid model resource filename");
    require(file.size()>=4 && file.substr(file.size()-4)==".glb","Only .glb models are supported");
    require(folder==modelFolderName(folder),"Invalid model folder path");
    const auto path=root+"/"+folder+"/";
    std::ifstream f(path+file,std::ios::binary);
    require(bool(f),"Missing selected model resource");
    f.seekg(0,std::ios::end); auto size=f.tellg();
    require(size>=64 && uint64_t(size)<=maxModelBytes,"Invalid model file size");
    auto bytes=read(f,0,size_t(size));
    auto model=decodeGlb(bytes);
    std::ifstream lighting(path+"lighting.txt");
    if(!lighting) return model;
    Lighting value; std::string extra;
    require(bool(lighting >> value.rim >> value.shadowOffset >> value.shadowRamp) && !(lighting >> extra),
            "Invalid lighting.txt in selected model folder");
    require(std::isfinite(value.rim) && std::isfinite(value.shadowOffset) && std::isfinite(value.shadowRamp),
            "Invalid model lighting values");
    if(value.shadowOffset<=0) value.shadowOffset=.12f;
    if(value.shadowRamp<=0) value.shadowRamp=.04f;
    model.lighting=value;
    return model;
}
void readModelRenderer(Model& model,const std::string& folder,const std::string& bundle,const std::string& shader,const std::string& error,const std::string& material) {
    if(model.rendererEmbedded) return;
    model.rendererBytes.clear();
    model.rendererShader=shader; model.rendererMaterial=material; model.rendererError=error;
    if(bundle.empty()) return;
    try {
        require(bundle==modelFolderName(bundle),"Invalid renderer filename");
        std::ifstream f(folder+"/"+bundle,std::ios::binary);
        require(bool(f),"Missing renderer bundle");
        f.seekg(0,std::ios::end); const auto size=f.tellg();
        require(size>=16 && uint64_t(size)<=16*1024*1024,"Invalid renderer size (limit 16 MiB)");
        model.rendererBytes=read(f,0,size_t(size));
        require(std::memcmp(model.rendererBytes.data(),"UnityFS\0",8)==0,"Expected Unity AssetBundle, not shader source");
    } catch(const std::exception& e) { model.rendererBytes.clear(); model.rendererError=e.what(); }
}
}
