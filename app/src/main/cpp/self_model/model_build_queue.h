#pragma once
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace ft::self_model {
enum class ModelBuildPhase { Capture,Rig,Physics,Root,Bone,Texture,MeshSetup,Mesh,Finish };
inline const char* modelBuildPhaseName(ModelBuildPhase phase) {
    switch(phase) {
        case ModelBuildPhase::Capture:return "capture";
        case ModelBuildPhase::Rig:return "rig";
        case ModelBuildPhase::Physics:return "physics";
        case ModelBuildPhase::Root:return "root";
        case ModelBuildPhase::Bone:return "bones";
        case ModelBuildPhase::Texture:return "textures";
        case ModelBuildPhase::MeshSetup:return "mesh_setup";
        case ModelBuildPhase::Mesh:return "meshes";
        case ModelBuildPhase::Finish:return "finish";
    }
    return "unknown";
}
struct ModelBuildTask { ModelBuildPhase phase; size_t index=0; };
class ModelBuildQueue {
    std::vector<ModelBuildTask> tasks_;
    size_t next_=0;
public:
    void reset(size_t bones,size_t textures,size_t meshes) {
        if(bones>8192 || textures>4096 || meshes>4096) throw std::runtime_error("Model build task limit exceeded");
        clear(); tasks_.reserve(bones+textures+meshes+6);
        for(auto phase:{ModelBuildPhase::Capture,ModelBuildPhase::Rig,ModelBuildPhase::Physics,ModelBuildPhase::Root})
            tasks_.push_back({phase,0});
        for(size_t i=0;i<bones;++i) tasks_.push_back({ModelBuildPhase::Bone,i});
        for(size_t i=0;i<textures;++i) tasks_.push_back({ModelBuildPhase::Texture,i});
        tasks_.push_back({ModelBuildPhase::MeshSetup,0});
        for(size_t i=0;i<meshes;++i) tasks_.push_back({ModelBuildPhase::Mesh,i});
        tasks_.push_back({ModelBuildPhase::Finish,0});
    }
    void clear() { tasks_.clear(); next_=0; }
    bool done() const { return next_==tasks_.size(); }
    size_t completed() const { return next_; }
    size_t total() const { return tasks_.size(); }


    template<class Clock,class Work> void advance(Clock clock,double budget,Work work) {
        advanceReady(clock,budget,[&](auto task) { work(task); return true; });
    }


    template<class Clock,class Work> void advanceReady(Clock clock,double budget,Work work) {
        const double start=clock();
        while(!done()) {
            if(!work(tasks_[next_])) break;
            ++next_;
            if(clock()-start>=budget) break;
        }
    }
};
}
