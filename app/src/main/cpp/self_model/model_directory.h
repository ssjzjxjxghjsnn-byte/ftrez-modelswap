#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ft::self_model {
struct DirectoryModel {
    std::string id,name,folder,stamp;
    std::string file="model.glb";
    std::string rendererBundle,rendererShader,rendererError;
    bool available=true;
    uint64_t revision=0;
    std::string rendererMaterial;
};
struct DirectoryScan {
    std::vector<DirectoryModel> models;
    std::string diagnostics;
};
DirectoryScan scanModelDirectory(const std::string& root);

class ModelDirectory {
    std::vector<DirectoryModel> entries_;
public:
    std::vector<int> update(const DirectoryScan& scan);
    const DirectoryModel* get(int key) const {
        return key>=0 && size_t(key)<entries_.size()?&entries_[key]:nullptr;
    }
    const std::vector<DirectoryModel>& entries() const { return entries_; }
};
}
