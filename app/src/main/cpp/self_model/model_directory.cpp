#include "self_model/model_directory.h"
#include "self_model/model_format.h"
#include "third_party/nlohmann/json.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <memory>
#include <fstream>
#include <map>
#include <algorithm>

namespace ft::self_model {
namespace {
struct FileInfo { bool directory=false,regular=false; uint64_t size=0; std::string stamp; };
FileInfo inspect(const std::string& path) {
    struct stat info{};
#ifdef _WIN32
    const int result=stat(path.c_str(),&info);
#else
    const int result=lstat(path.c_str(),&info);
#endif
    if(result!=0) return {};
    FileInfo out;
    out.directory=(info.st_mode&S_IFMT)==S_IFDIR;
    out.regular=(info.st_mode&S_IFMT)==S_IFREG;
    out.size=uint64_t(info.st_size);
    out.stamp=std::to_string(out.size)+":"+std::to_string(info.st_mtime);
#ifndef _WIN32
    out.stamp+=":"+std::to_string(info.st_mtim.tv_nsec);
#endif
    return out;
}
}
DirectoryScan scanModelDirectory(const std::string& root) {
    DirectoryScan result;
    auto error=[&](const std::string& text) { if(!result.diagnostics.empty()) result.diagnostics+=" | "; result.diagnostics+=text; };
    try {
        std::map<std::string,int> counts;
        std::unique_ptr<DIR,int(*)(DIR*)> directory(opendir(root.c_str()),closedir);
        if(!directory) throw std::runtime_error("Cannot open model directory: "+root);
        while(auto* dir=readdir(directory.get())) {
            const std::string folder=dir->d_name;
            if(folder=="." || folder==".." || !inspect(root+"/"+folder).directory) continue;
            try {
                if(folder!=modelFolderName(folder)) throw std::runtime_error("Invalid folder name");
                const auto metadata=root+"/"+folder+"/model.json";
                const auto info=inspect(metadata);
                if(!info.regular || info.size>65536) throw std::runtime_error("Invalid model.json size/path");
                std::ifstream stream(metadata,std::ios::binary);
                std::string raw(size_t(info.size),'\0');
                stream.read(raw.data(),raw.size());
                if(!stream || stream.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("model.json changed during read");
                auto json=nlohmann::json::parse(raw);
                DirectoryModel entry;
                entry.id=json.at("id").get<std::string>(); entry.name=json.at("name").get<std::string>(); entry.folder=folder;
                auto valid=[](const std::string& text,size_t limit) {
                    return !text.empty() && text.size()<=limit && std::none_of(text.begin(),text.end(),[](unsigned char c){return c<32;});
                };
                if(!valid(entry.id,128) || !valid(entry.name,256)) throw std::runtime_error("Invalid id/name");
                entry.stamp=raw;
                entry.file=json.value("model",std::string{});
                if(entry.file.empty()) entry.file="model.glb";
                if(entry.file!=modelFolderName(entry.file)) throw std::runtime_error("Invalid model filename");
                if(entry.file.size()<4 || entry.file.substr(entry.file.size()-4)!=".glb")
                    throw std::runtime_error("Only .glb models are supported");
                for(const auto& file:std::vector<std::string>{entry.file,"lighting.txt"}) {
                    const auto resource=inspect(root+"/"+folder+"/"+file);
                    if(file=="lighting.txt" && !resource.regular) {entry.stamp+="/default-lighting";continue;}
                    if(!resource.regular) throw std::runtime_error(std::string("Missing resource: ")+file);
                    entry.stamp+="/"+resource.stamp;
                }

                try {
                    entry.rendererBundle="renderer.bundle";
                    if(json.contains("renderer")) {
                        const auto& renderer=json.at("renderer");
                        entry.rendererBundle=renderer.value("bundle",std::string("renderer.bundle"));
                        entry.rendererShader=renderer.value("shader",std::string{});
                        entry.rendererMaterial=renderer.value("material",std::string{});
                    }
                    if(entry.rendererBundle.empty() || entry.rendererBundle!=modelFolderName(entry.rendererBundle))
                        throw std::runtime_error("Invalid renderer bundle filename");
                    if(entry.rendererShader.size()>512 || entry.rendererShader.find('\0')!=std::string::npos)
                        throw std::runtime_error("Invalid renderer shader path");
                    if(entry.rendererMaterial.size()>512 || entry.rendererMaterial.find('\0')!=std::string::npos)
                        throw std::runtime_error("Invalid renderer material path");
                    const auto bundle=inspect(root+"/"+folder+"/"+entry.rendererBundle);
                    entry.stamp+="/renderer:"+bundle.stamp;
                    if(!bundle.regular || bundle.size<16 || bundle.size>16*1024*1024)
                        throw std::runtime_error("Renderer bundle missing or invalid (limit 16 MiB)");
                } catch(const std::exception& e) {
                    entry.rendererError=e.what(); entry.rendererBundle.clear();
                }
                ++counts[entry.id];
                result.models.push_back(std::move(entry));
            } catch(const std::exception& e) { error(folder+": "+e.what()); }
        }
        result.models.erase(std::remove_if(result.models.begin(),result.models.end(),[&](const auto& entry) {
            if(counts[entry.id]==1) return false;
            error("Duplicate model ID: "+entry.id); return true;
        }),result.models.end());
        std::sort(result.models.begin(),result.models.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    } catch(const std::exception& e) { result.models.clear(); error(e.what()); }
    return result;
}
std::vector<int> ModelDirectory::update(const DirectoryScan& scan) {
    std::vector<int> changed;
    for(size_t i=0;i<entries_.size();++i) {
        auto& old=entries_[i];
        auto found=std::find_if(scan.models.begin(),scan.models.end(),[&](const auto& e){return e.id==old.id;});
        if(found==scan.models.end()) {
            if(old.available) { old.available=false; ++old.revision; changed.push_back(int(i)); }
        } else if(!old.available || old.stamp!=found->stamp || old.folder!=found->folder) {
            auto revision=old.revision+1; old=*found; old.revision=revision; changed.push_back(int(i));
        }
    }
    for(const auto& entry:scan.models) {
        if(std::none_of(entries_.begin(),entries_.end(),[&](const auto& old){return old.id==entry.id;})) {
            changed.push_back(int(entries_.size())); entries_.push_back(entry); entries_.back().revision=1;
        }
    }
    return changed;
}
}
