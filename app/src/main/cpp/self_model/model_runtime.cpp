#ifdef FT_MODEL_TOON_BUNDLE
extern "C" const unsigned char ft_model_toon_start[], ft_model_toon_end[];
#endif
#include "self_model/model_runtime.h"
#include "self_model/lobby_render.h"
#include "self_model/unity_api.h"
#include "self_model/model_pose.h"
#include "self_model/model_dynamics.h"
#include "self_model/model_surface_collision.h"
#include "self_model/model_texture_plan.h"
#include "self_model/model_texture_upload.h"
#include "self_model/model_transform_access.h"
#include "self_model/model_bone_updates.h"
#include "self_model/model_build_queue.h"
#include "self_model/model_attachment.h"
#include "self_model/model_visibility.h"
#include "self_model/model_switch.h"
#include "self_model/game_model_binding.h"
#include "self_model/model_hooks.h"
#include "self_model/switch_request.h"
#include "self_model/model_cache.h"
#include "self_model/model_snapshot.h"
#include "self_model/model_shared_pool.h"
#include "self_model/model_mesh_sharing.h"
#include "self_model/model_directory.h"
#include "self_model/bone_mapping.h"
#include "self_model/host_skeleton.h"
#include "self_model/model_style.h"
#include "self_model/model_gltf_material.h"
#include "self_model/rabbit_model.h"
#include "self_model/model_fit.h"
#include "self_model/model_arm_avoidance.h"
#include "self_model/host_pose.h"
#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <memory>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace ft::self_model {
namespace { sdk::Symbols modelSymbols; ModelHooks modelHooks; void consumeSwitchRequests(); }
void* profileObject() { return modelHooks.profileObject(); }
void prepareBindings(sdk::Symbols symbols) { modelSymbols=std::move(symbols); }
void installMethodHooks(const std::vector<void*>& images,std::function<bool()> mainThread,std::function<void()> afterUpdate) {
    modelHooks.install(images,modelSymbols,std::move(mainThread),[afterUpdate=std::move(afterUpdate)] {
        consumeSwitchRequests();
        afterUpdate();
    });
}
namespace {
std::string resourcePath="/storage/emulated/0/Android/data/com.pi.czrxdfirst/files/ftrez-models";
struct LoadSlot {
    std::string message="Disabled; original model";
    std::shared_ptr<const Model> asset;
    int assetIndex=-1,requestedIndex=-1;
};
struct State {
    std::mutex mutex;
    SwitchRequest<SelfModelOptions> switches;
    uint64_t refresh=0;
    std::array<LoadSlot,3> slots;
    ModelCache<Model> cache{64*1024*1024,4};
    std::map<int,std::weak_ptr<const Model>> sharedSources;
    std::set<int> loading,pendingLoads;
    bool loaderRunning=false,scanning=false;
    ModelDirectory directory;
    std::string scanMessage;
    std::chrono::steady_clock::time_point nextScan{};
};
State& state() { static auto* s=new State; return *s; }
std::shared_ptr<const Model> cachedAsset(State& s,int key) {
    if(auto found=s.cache.get(key)) return found;
    const auto at=s.sharedSources.find(key);
    return at==s.sharedSources.end()?nullptr:at->second.lock();
}
void report(size_t instance,std::string text) {
    auto& s=state(); std::lock_guard lock(s.mutex);
    auto& slot=s.slots[instance];
    if(slot.message==text) return;
    slot.message=std::move(text);
    __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","%s: %s",instance==1?"Rabbit":instance==2?"Preview":"Self",slot.message.c_str());
}
void consumeSwitchRequests() {
    auto& s=state(); std::lock_guard lock(s.mutex);
    if(s.switches.consume()) ++s.refresh;
}
SelfModelOptions settings() { auto& s=state(); std::lock_guard lock(s.mutex); return s.switches.requested(); }
size_t modelBytes(const Model& model) {
    size_t bytes=sizeof(model)+model.bones.capacity()*sizeof(Bone)+model.colliders.capacity()*sizeof(Collider)+
        model.vertices.capacity()*sizeof(Vertex)+model.indices.capacity()*sizeof(uint32_t)+
        model.materials.capacity()*sizeof(Material)+model.parts.capacity()*sizeof(Part)+
        model.textureSources.capacity()*sizeof(int32_t);
    for(const auto& b:model.bones) bytes+=b.name.capacity();
    for(const auto& m:model.materials) bytes+=m.name.capacity()+m.png.capacity();
    for(const auto& p:model.parts) bytes+=p.name.capacity();
    for(const auto& image:model.images) bytes+=sizeof(ModelImage)+image.png.capacity();
    return bytes+model.rendererBytes.capacity()+model.rendererShader.capacity()+model.rendererError.capacity()+model.rendererMaterial.capacity();
}

void requestAsset(State& s,int selected) {
    const auto* entry=s.directory.get(selected);
    if(!entry || !entry->available) return;
    if(cachedAsset(s,selected) || !s.loading.insert(selected).second) return;
    s.pendingLoads.insert(selected);
    if(s.loaderRunning) return;
    s.loaderRunning=true;
    try {
        std::thread([] {
            for(;;) {
                int selected; DirectoryModel entry;
                {
                    auto& s=state(); std::lock_guard lock(s.mutex);


                    for(auto it=s.pendingLoads.begin();it!=s.pendingLoads.end();) {
                        const bool wanted=std::any_of(s.slots.begin(),s.slots.end(),[&](const auto& slot) {
                            return slot.requestedIndex==*it;
                        });
                        if(!wanted) { s.loading.erase(*it); it=s.pendingLoads.erase(it); }
                        else ++it;
                    }
                    if(s.pendingLoads.empty()) { s.loaderRunning=false; return; }
                    const auto requested=s.switches.requested().model;
                    selected=s.pendingLoads.count(requested)?requested:*s.pendingLoads.begin();
                    s.pendingLoads.erase(selected);
                    const auto* current=s.directory.get(selected);
                    if(!current || !current->available) { s.loading.erase(selected); continue; }
                    entry=*current;
                }
                std::shared_ptr<const Model> asset; std::string error;
                const auto readStarted=std::chrono::steady_clock::now();
                try {
                    auto loaded=readModelDirectory(resourcePath,entry.folder,entry.file);
                    const auto parsed=std::chrono::steady_clock::now();
                    readModelRenderer(loaded,std::string(resourcePath)+"/"+entry.folder,entry.rendererBundle,entry.rendererShader,entry.rendererError,entry.rendererMaterial);
                    loaded.textureSources=modelTextureSources(loaded);
                    __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD",
                        "read model=%d folder=%s parse_ms=%.3f renderer_plan_ms=%.3f cpu_bytes=%zu renderer_bytes=%zu renderer_error=%s",
                        selected,entry.folder.c_str(),std::chrono::duration<double,std::milli>(parsed-readStarted).count(),
                        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-parsed).count(),
                        modelBytes(loaded),loaded.rendererBytes.size(),loaded.rendererError.c_str());
                    asset=std::make_shared<Model>(std::move(loaded));
                }
                catch(const std::exception& e) {
                    error=e.what();
                    __android_log_print(ANDROID_LOG_ERROR,"FTREZ-MODEL-LOAD","read failed model=%d folder=%s ms=%.3f reason=%s",
                        selected,entry.folder.c_str(),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-readStarted).count(),error.c_str());
                }
                auto& s=state(); std::lock_guard lock(s.mutex);
                s.loading.erase(selected);
                const auto* current=s.directory.get(selected);
                if(!current || !current->available || current->revision!=entry.revision) {
                    requestAsset(s,selected); continue;
                }
                if(asset) s.cache.put(selected,asset,modelBytes(*asset));
                for(auto& slot:s.slots) if(slot.requestedIndex==selected) {
                    slot.asset=asset; slot.assetIndex=selected;
                    slot.message=error.empty()?"Model ready; switch queued for method hook":error;
                }
                ++s.refresh;
            }
        }).detach();
    } catch(...) { s.loaderRunning=false; s.loading.erase(selected); s.pendingLoads.erase(selected); throw; }
}

void refreshDirectory() {
    auto& s=state(); std::lock_guard lock(s.mutex);
    const auto now=std::chrono::steady_clock::now();
    if(s.scanning || now<s.nextScan) return;
    s.scanning=true; s.nextScan=now+std::chrono::seconds(3);
    try {
        std::thread([] {
            auto scan=scanModelDirectory(resourcePath);
            auto& s=state(); std::lock_guard lock(s.mutex);
            const auto changed=s.directory.update(scan);
            s.scanMessage=scan.diagnostics; s.scanning=false;
            for(int key:changed) {
                s.cache.erase(key);
                s.sharedSources.erase(key);
                for(auto& slot:s.slots) if(slot.requestedIndex==key) {
                    slot.asset.reset(); slot.assetIndex=-1;
                    const auto* entry=s.directory.get(key);
                    slot.message=entry && entry->available?"Model updated; loading":"Selected model unavailable";
                    requestAsset(s,key);
                }
            }
            if(!changed.empty()) ++s.refresh;
        }).detach();
    } catch(...) { s.scanning=false; s.scanMessage="Cannot start model directory scan"; }
}
std::vector<SelfModelChoice> choices() {
    refreshDirectory();
    auto& s=state(); std::lock_guard lock(s.mutex);
    std::vector<SelfModelChoice> result;
    for(size_t i=0;i<s.directory.entries().size();++i) {
        const auto& e=s.directory.entries()[i];
        if(e.available) result.push_back({int(i),e.id,e.name});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return std::tie(a.label,a.id)<std::tie(b.label,b.id);});
    return result;
}
std::string modelLabel(int key) {
    auto& s=state(); std::lock_guard lock(s.mutex);
    const auto* e=s.directory.get(key); return e?e->name:"Unavailable model";
}
bool modelAvailable(int key) {
    auto& s=state(); std::lock_guard lock(s.mutex);
    const auto* e=s.directory.get(key); return e && e->available;
}
void configure(SelfModelOptions v) {
    auto& s=state(); std::lock_guard lock(s.mutex); v=normalizeSelfModel(v);
    const auto previous=s.switches.requested();
    if(!s.switches.submit(v)) return;
    for(size_t i=0;i<s.slots.size();++i) {
        auto& slot=s.slots[i];
        const auto next=modelInstanceOptions(v,i==1),old=modelInstanceOptions(previous,i==1);
        if(next.enabled!=old.enabled || next.model!=old.model) {
            slot.requestedIndex=next.model; slot.asset=cachedAsset(s,next.model);
            slot.assetIndex=slot.asset?next.model:-1;

            if(next.enabled || next.model!=old.model) requestAsset(s,next.model);
            slot.message=next.enabled?"Switch queued for method hook":"Restore queued for method hook";
        } else if(next.style!=old.style || next.hairTone!=old.hairTone)
            slot.message="Applying "+std::string(styleName(next.style));
    }
    __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","Queued hook switch self=%d model=%d rabbit=%d rabbitModel=%d",v.enabled,v.model,v.rabbitEnabled,v.rabbitModel);
}
std::shared_ptr<const Model> resource(size_t instance,int selected) {
    auto& s=state(); std::lock_guard lock(s.mutex);
    auto& slot=s.slots[instance];


    if(modelInstanceOptions(s.switches.requested(),instance==1).model!=selected) return {};
    if(slot.assetIndex==selected) return slot.asset;
    if(auto cached=cachedAsset(s,selected)) {
        slot.assetIndex=selected; slot.asset=cached; return cached;
    }
    slot.requestedIndex=selected;
    requestAsset(s,selected);
    return {};
}
Matrix stored(glm::mat4 m) { Matrix out; std::memcpy(out.m.data(),glm::value_ptr(m),64); return out; }
double seconds();
struct ModelTemps {
    UnityApi& api;
    ~ModelTemps() { if(api.gchandle_free) api.clearTemps(); }
};
// Own the Unity allocations independently of every display/animation instance.
// The pool retains the two most recent models across scene and slot teardown.
struct SharedModelResources {
    struct Style {
        std::vector<std::vector<uint32_t>> materials;
        std::string shader,notice;
        uint64_t generation=0;
        bool lobby=false;
        double retryAfter=0;
    };
    UnityApi api;
    void *alive,*destroy;
    std::vector<uint32_t> owned;
    std::vector<std::optional<uint32_t>> textures;
    std::vector<std::array<uint32_t,2>> meshes;
    std::map<std::pair<int,int>,Style> styles;
    uint32_t shader=0,material=0;
    uint32_t hostMaterial=0;
    bool hostMaterialLobby=false;
    uint64_t generation=0;
    explicit SharedModelResources(const UnityApi& source,void* isAlive,void* destroyObject,const Model& model)
        :api(source),alive(isAlive),destroy(destroyObject),textures(modelTextureCount(model)),meshes(model.parts.size()) {
        // Copy the binding table, never ownership of the caller's temporary GC roots.
        api.temporaries.clear();
    }
    ~SharedModelResources() {
        for(auto it=owned.rbegin();it!=owned.rend();++it) {
            try {
                auto object=api.gchandle_get_target(*it);
                if(object && api.value<bool>(alive,nullptr,{object})) api.call(destroy,nullptr,{object});
            } catch(const std::exception& e) {
                __android_log_print(ANDROID_LOG_WARN,"FTREZ-MODEL-SHARED","release: %s",e.what());
            }
            api.gchandle_free(*it); api.clearTemps();
        }
    }
};
ModelSharedPool<Model,SharedModelResources>& sharedModels() {
    static auto* pool=new ModelSharedPool<Model,SharedModelResources>(2); return *pool;
}
struct MeshPart {
    uint32_t mesh,renderer;
    std::vector<uint32_t> arms;
    uint32_t vertices=0,normals=0;
    std::vector<Vec3> cpuVertices,cpuNormals;
    uint64_t contactRevision=0;
    bool sharedMesh=false;
};
struct SavedAnimator { uint32_t handle; int culling; bool changed=false; };
struct Attachment { uint32_t transform,parent; size_t bone; AttachmentPosition position; };
struct Runtime {
    explicit Runtime(size_t index):instance(index) {}
    size_t instance;
    bool rabbit() const { return instance==1; }
    GameModelBinding game;
    std::optional<PlayerView> playerView;
    bool appliedFirstPerson=false;
    uint32_t previewUI=0;

    UnityApi api;
    ModelTransformAccess transforms;
    ModelTextureUpload textureUpload;
    std::shared_ptr<ModelTextureRequest> textureRequest;
    size_t textureRequestSource=0;
    std::unique_ptr<Runtime> pending;
    ModelBuildQueue buildQueue;
    std::shared_ptr<SharedModelResources> shared;
    bool lobbyHost=false;
    double nextLobbyCheck=0;
    void* lobbyCheckedHost=nullptr; // Identity only; never dereferenced.
    uint64_t sharedStyleGeneration=0;
    uint32_t buildBoneArray=0,buildBindArray=0;
    int buildLayer=0;
    double buildStarted=0,buildCpuMs=0,pendingRetry=0;
    const char* buildPhase=nullptr;
    struct UpdateStats {
        double since=0,totalMs=0,maxMs=0,topologyMs=0,surfaceMs=0,boneWriteMs=0;
        double surfaceWaitMs=0,surfaceOverlapMs=0;
        uint64_t surfaceAsyncFrames=0;
        uint64_t speculativeTriangles=0,recomputedTriangles=0;
        uint64_t boneWrites=0,surfaceReusedFrames=0,contactUploads=0;
        uint64_t frames=0,calls=0,arrays=0,bytes=0;
    } perf;
    void report(std::string message) { self_model::report(instance,std::move(message)); }
    bool bound=false,gpu=false,restorePending=false;
    uint64_t lastRefresh=0;
    int lastFrame=-1,selected=-1,appliedStyle=-1,appliedHairTone=-1;
    unsigned rejectedPoses=0;
    double retry=0;
    std::shared_ptr<const Model> asset;
    uint32_t host=0,root=0,rootTransform=0;
    uint32_t carrier=0,carrierTransform=0;
    bool snapshotParked=false,carrierLinked=false;
    std::vector<uint32_t> hostRetained;
    std::vector<uint32_t> owned,retained,boneRefs,hostBones;
    ModelBoneUpdates boneUpdates;
    std::vector<std::optional<Matrix>> rest;
    std::vector<SavedRenderer> original;
    std::vector<SavedAnimator> animators;
    std::vector<MeshPart> parts;
    std::vector<uint32_t> textures,styleObjects;
    std::string shaderDescription,styleNotice;
    float fit=1;
    BodyLengths modelLengths,hostLengths;
    HumanPositions modelPositions;
    size_t matched=0;
    ModelDynamics dynamics;
    ModelSurfaceCollision surfaceCollision;
    std::unique_ptr<ModelCollisionWorker> surfaceWorker;
    ArmAvoidance armAvoidance;
    double lastPoseTime=0,nextRabbitSearch=0,nextStyleProbe=0;
    float rabbitBaseline=0;
    uint32_t rabbitTarget=0;
    void* sceneObjects=nullptr;
    void* sceneObjectsInactive=nullptr;
    void* mainCamera=nullptr;
    bool firstPerson=false;
    int headIndex=-1,neckIndex=-1;
    float bodyHeight=0,headRadius=0;
    using Topology=std::vector<std::pair<uintptr_t,uintptr_t>>;
    Topology topology;
    double nextTopology=0;
    std::vector<Attachment> attachments;
    std::set<uintptr_t> skeletonObjects;
    void* getPosition=nullptr;
    void* getBounds=nullptr;
    void *alive=nullptr,*destroy=nullptr,*goCtor=nullptr,*getTransform=nullptr,*componentGO=nullptr;
    void *dontDestroy=nullptr,*instantiate=nullptr;
    void *getHideFlags=nullptr,*setHideFlags=nullptr;
    void *getLayer=nullptr,*setLayer=nullptr,*setActive=nullptr,*activeHierarchy=nullptr,*find=nullptr;
    void *setParent=nullptr,*getParent=nullptr,*setPosition=nullptr,*setRotation=nullptr,*setScale=nullptr,*localToWorld=nullptr,*worldToLocal=nullptr;
    void *getComponent=nullptr;
    void *components=nullptr,*getName=nullptr,*getEnabled=nullptr,*setEnabled=nullptr,*addComponent=nullptr;
    void *meshCtor=nullptr,*setVertices=nullptr,*setNormals=nullptr,*setUV=nullptr,*setTriangles=nullptr,*bounds=nullptr,*markDynamic=nullptr;
    void *smrMesh=nullptr,*smrBones=nullptr,*meshBind=nullptr,*setBind=nullptr,*setWeights=nullptr;
    void *setBones=nullptr,*setRootBone=nullptr,*setSharedMesh=nullptr,*offscreen=nullptr,*filterMesh=nullptr;
    void *shaderFind=nullptr,*matCtor=nullptr,*setTexture=nullptr,*setColor=nullptr,*setMaterial=nullptr;
    void *texCtor=nullptr,*loadImage=nullptr,*texFullCtor=nullptr;
    void *setWrapU=nullptr,*setWrapV=nullptr,*setFilter=nullptr,*setVector=nullptr,*setQueue=nullptr;
    void *matClone=nullptr,*getMaterials=nullptr,*setMaterials=nullptr,*getShader=nullptr,*supported=nullptr;
    void *hasProperty=nullptr,*setFloat=nullptr,*setNamedColor=nullptr,*setNamedTexture=nullptr,*enableKeyword=nullptr;
    void *findObjects=nullptr,*loadedBundles=nullptr,*bundleNames=nullptr,*loadAsset=nullptr;
    void *isHuman=nullptr,*humanBone=nullptr;
    void *getCulling=nullptr,*setCulling=nullptr;
    void bind(const std::vector<void*>& images) {
        api.bind(images);
        sceneObjectsInactive=api.method("UnityEngine.Object","FindObjectsOfType",{"System.Type","System.Boolean"},"UnityEngine.Object[]",true);
        sceneObjects=api.method("UnityEngine.Object","FindObjectsOfType",{"System.Type"},"UnityEngine.Object[]",true);
        mainCamera=api.method("UnityEngine.Camera","get_main",{},"UnityEngine.Camera",true,true);
        alive=api.method("UnityEngine.Object","op_Implicit",{"UnityEngine.Object"},"System.Boolean",true);
        destroy=api.method("UnityEngine.Object","Destroy",{"UnityEngine.Object"},"System.Void",true);
        dontDestroy=api.method("UnityEngine.Object","DontDestroyOnLoad",{"UnityEngine.Object"},"System.Void",true);
        instantiate=api.method("UnityEngine.Object","Instantiate",{"UnityEngine.Object"},"UnityEngine.Object",true);
        getHideFlags=api.method("UnityEngine.Object","get_hideFlags",{},"UnityEngine.HideFlags");
        setHideFlags=api.method("UnityEngine.Object","set_hideFlags",{"UnityEngine.HideFlags"},"System.Void");
        getName=api.method("UnityEngine.Object","get_name",{},"System.String");
        goCtor=api.method("UnityEngine.GameObject",".ctor",{"System.String"},"System.Void");
        getTransform=api.method("UnityEngine.GameObject","get_transform",{},"UnityEngine.Transform");
        componentGO=api.method("UnityEngine.Component","get_gameObject",{},"UnityEngine.GameObject");
        find=api.method("UnityEngine.GameObject","Find",{"System.String"},"UnityEngine.GameObject",true);
        addComponent=api.method("UnityEngine.GameObject","AddComponent",{"System.Type"},"UnityEngine.Component");
        getLayer=api.method("UnityEngine.GameObject","get_layer",{},"System.Int32");
        setLayer=api.method("UnityEngine.GameObject","set_layer",{"System.Int32"},"System.Void");
        setActive=api.method("UnityEngine.GameObject","SetActive",{"System.Boolean"},"System.Void");
        activeHierarchy=api.method("UnityEngine.GameObject","get_activeInHierarchy",{},"System.Boolean");
        setParent=api.method("UnityEngine.Transform","SetParent",{"UnityEngine.Transform","System.Boolean"},"System.Void");
        getParent=api.method("UnityEngine.Transform","get_parent",{},"UnityEngine.Transform");
        setPosition=api.method("UnityEngine.Transform","set_localPosition",{"UnityEngine.Vector3"},"System.Void");
        getPosition=api.method("UnityEngine.Transform","get_localPosition",{},"UnityEngine.Vector3");
        setRotation=api.method("UnityEngine.Transform","set_localRotation",{"UnityEngine.Quaternion"},"System.Void");
        setScale=api.method("UnityEngine.Transform","set_localScale",{"UnityEngine.Vector3"},"System.Void");
        localToWorld=api.method("UnityEngine.Transform","get_localToWorldMatrix",{},"UnityEngine.Matrix4x4");
        worldToLocal=api.method("UnityEngine.Transform","get_worldToLocalMatrix",{},"UnityEngine.Matrix4x4");
        transforms.bind(api);
        getComponent=api.method("UnityEngine.Component","GetComponent",{"System.Type"},"UnityEngine.Component");
        components=api.method("UnityEngine.Component","GetComponentsInChildren",{"System.Type","System.Boolean"},"UnityEngine.Component[]");
        getEnabled=api.method("UnityEngine.Renderer","get_enabled",{},"System.Boolean");
        getBounds=api.method("UnityEngine.Renderer","get_bounds",{},"UnityEngine.Bounds");
        setEnabled=api.method("UnityEngine.Renderer","set_enabled",{"System.Boolean"},"System.Void");
        meshCtor=api.method("UnityEngine.Mesh",".ctor",{},"System.Void");
        setVertices=api.method("UnityEngine.Mesh","set_vertices",{"UnityEngine.Vector3[]"},"System.Void");
        setNormals=api.method("UnityEngine.Mesh","set_normals",{"UnityEngine.Vector3[]"},"System.Void");
        setUV=api.method("UnityEngine.Mesh","set_uv",{"UnityEngine.Vector2[]"},"System.Void");
        setTriangles=api.method("UnityEngine.Mesh","set_triangles",{"System.Int32[]"},"System.Void");
        bounds=api.method("UnityEngine.Mesh","RecalculateBounds",{},"System.Void");
        markDynamic=api.method("UnityEngine.Mesh","MarkDynamic",{},"System.Void",false,true);
        smrMesh=api.method("UnityEngine.SkinnedMeshRenderer","get_sharedMesh",{},"UnityEngine.Mesh");
        smrBones=api.method("UnityEngine.SkinnedMeshRenderer","get_bones",{},"UnityEngine.Transform[]");
        meshBind=api.method("UnityEngine.Mesh","get_bindposes",{},"UnityEngine.Matrix4x4[]");
        filterMesh=api.method("UnityEngine.MeshFilter","set_sharedMesh",{"UnityEngine.Mesh"},"System.Void");
        setSharedMesh=api.method("UnityEngine.SkinnedMeshRenderer","set_sharedMesh",{"UnityEngine.Mesh"},"System.Void",false,true);
        setBind=api.method("UnityEngine.Mesh","set_bindposes",{"UnityEngine.Matrix4x4[]"},"System.Void",false,true);
        setWeights=api.method("UnityEngine.Mesh","set_boneWeights",{"UnityEngine.BoneWeight[]"},"System.Void",false,true);
        setBones=api.method("UnityEngine.SkinnedMeshRenderer","set_bones",{"UnityEngine.Transform[]"},"System.Void",false,true);
        setRootBone=api.method("UnityEngine.SkinnedMeshRenderer","set_rootBone",{"UnityEngine.Transform"},"System.Void",false,true);
        offscreen=api.method("UnityEngine.SkinnedMeshRenderer","set_updateWhenOffscreen",{"System.Boolean"},"System.Void",false,true);
        gpu=setSharedMesh&&setBind&&setWeights&&setBones&&setRootBone&&offscreen;
        shaderFind=api.method("UnityEngine.Shader","Find",{"System.String"},"UnityEngine.Shader",true);
        matCtor=api.method("UnityEngine.Material",".ctor",{"UnityEngine.Shader"},"System.Void");
        setTexture=api.method("UnityEngine.Material","set_mainTexture",{"UnityEngine.Texture"},"System.Void");
        setColor=api.method("UnityEngine.Material","set_color",{"UnityEngine.Color"},"System.Void");
        setMaterial=api.method("UnityEngine.Renderer","set_sharedMaterial",{"UnityEngine.Material"},"System.Void");
        matClone=api.method("UnityEngine.Material",".ctor",{"UnityEngine.Material"},"System.Void");
        getMaterials=api.method("UnityEngine.Renderer","get_sharedMaterials",{},"UnityEngine.Material[]");
        setMaterials=api.method("UnityEngine.Renderer","set_sharedMaterials",{"UnityEngine.Material[]"},"System.Void");
        getShader=api.method("UnityEngine.Material","get_shader",{},"UnityEngine.Shader");
        supported=api.method("UnityEngine.Shader","get_isSupported",{},"System.Boolean");
        hasProperty=api.method("UnityEngine.Material","HasProperty",{"System.String"},"System.Boolean");
        setFloat=api.method("UnityEngine.Material","SetFloat",{"System.String","System.Single"},"System.Void");
        setNamedColor=api.method("UnityEngine.Material","SetColor",{"System.String","UnityEngine.Color"},"System.Void");
        setNamedTexture=api.method("UnityEngine.Material","SetTexture",{"System.String","UnityEngine.Texture"},"System.Void");
        enableKeyword=api.method("UnityEngine.Material","EnableKeyword",{"System.String"},"System.Void");
        findObjects=api.method("UnityEngine.Resources","FindObjectsOfTypeAll",{"System.Type"},"UnityEngine.Object[]",true);
        loadedBundles=api.method("UnityEngine.AssetBundle","GetAllLoadedAssetBundles_Native",{},"UnityEngine.AssetBundle[]",true,true);
        bundleNames=api.method("UnityEngine.AssetBundle","GetAllAssetNames",{},"System.String[]",false,true);
        loadAsset=api.method("UnityEngine.AssetBundle","LoadAsset",{"System.String","System.Type"},"UnityEngine.Object",false,true);
        texCtor=api.method("UnityEngine.Texture2D",".ctor",{"System.Int32","System.Int32"},"System.Void");
        loadImage=api.method("UnityEngine.ImageConversion","LoadImage",{"UnityEngine.Texture2D","System.Byte[]","System.Boolean"},"System.Boolean",true);
        texFullCtor=api.method("UnityEngine.Texture2D",".ctor",{"System.Int32","System.Int32","UnityEngine.TextureFormat","System.Boolean","System.Boolean"},"System.Void");
        setWrapU=api.method("UnityEngine.Texture","set_wrapModeU",{"UnityEngine.TextureWrapMode"},"System.Void");
        setWrapV=api.method("UnityEngine.Texture","set_wrapModeV",{"UnityEngine.TextureWrapMode"},"System.Void");
        setFilter=api.method("UnityEngine.Texture","set_filterMode",{"UnityEngine.FilterMode"},"System.Void");
        setVector=api.method("UnityEngine.Material","SetVector",{"System.String","UnityEngine.Vector4"},"System.Void");
        setQueue=api.method("UnityEngine.Material","set_renderQueue",{"System.Int32"},"System.Void");
        textureUpload.bind(api);
        isHuman=api.method("UnityEngine.Animator","get_isHuman",{},"System.Boolean",false,true);
        humanBone=api.method("UnityEngine.Animator","GetBoneTransform",{"UnityEngine.HumanBodyBones"},"UnityEngine.Transform",false,true);
        getCulling=api.method("UnityEngine.Animator","get_cullingMode",{},"UnityEngine.AnimatorCullingMode");
        setCulling=api.method("UnityEngine.Animator","set_cullingMode",{"UnityEngine.AnimatorCullingMode"},"System.Void");
        if(!rabbit()) game.bind(api,images,modelSymbols);
        bound=true;
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD",
            "bindings instance=%zu staged_build=1 gpu=%d local_matrix_out=%d inverse_matrix_out=%d combined_pose=%d texture_type=%d background_texture_decode=%d",
            instance,gpu,transforms.localToWorldOut!=nullptr,transforms.worldToLocalOut!=nullptr,
            transforms.positionAndRotation!=nullptr,api.classes["UnityEngine.Texture"]!=nullptr,textureUpload.ready());
    }
    void* ref(uint32_t h) { return h?api.gchandle_get_target(h):nullptr; }
    bool live(void* obj) { return obj && api.value<bool>(alive,nullptr,{obj}); }
    uint32_t retain(void* obj,bool own=false) {
        if(!obj) throw std::runtime_error("Unity object creation failed");
        auto h=api.gchandle_new(obj,false); if(!h) throw std::runtime_error("Cannot retain model object");
        (own?owned:retained).push_back(h); return h;
    }
    void release(uint32_t handle) {
        api.gchandle_free(handle);
        retained.erase(std::remove(retained.begin(),retained.end(),handle),retained.end());
        hostRetained.erase(std::remove(hostRetained.begin(),hostRetained.end(),handle),hostRetained.end());
    }
    uint32_t retainHost(void* obj) {
        auto handle=retain(obj); hostRetained.push_back(handle); return handle;
    }
    void* create(const char* type,void* ctor,std::initializer_list<void*> args={}) {
        auto obj=api.hold(api.object_new(api.klass(type)));
        if(!obj) throw std::runtime_error("Cannot allocate model object");

        retain(obj,true); api.call(ctor,obj,args); return obj;
    }
    uint32_t shareOwned(void* object) {
        auto it=std::find_if(owned.begin(),owned.end(),[&](auto handle){return ref(handle)==object;});
        if(it==owned.end()) throw std::runtime_error("Shared model object has no owner");
        preserveShared(object);
        auto handle=*it; shared->owned.push_back(handle); owned.erase(it); return handle;
    }
    void preserveShared(void* object) {
        // Retain across Resources.UnloadUnusedAssets while no slot is visible.
        // Pool eviction explicitly destroys these allocations on the main hook.
        int flags=api.value<int>(getHideFlags,object)|32; // DontUnloadUnusedAsset
        api.call(setHideFlags,object,{&flags});
    }
    void shareOwnedFrom(size_t begin) {
        for(auto it=owned.begin()+begin;it!=owned.end();++it) preserveShared(ref(*it));
        shared->owned.insert(shared->owned.end(),owned.begin()+begin,owned.end());
        owned.resize(begin);
    }
    void releaseLocalStyle() {
        for(auto h:styleObjects) {
            if(live(ref(h))) api.call(destroy,nullptr,{ref(h)});
            api.gchandle_free(h); auto it=std::find(owned.begin(),owned.end(),h);
            if(it!=owned.end()) *it=0;
        }
        styleObjects.clear(); owned.erase(std::remove(owned.begin(),owned.end(),0),owned.end());
    }
    void* gameObject(const std::string& name,void* parent,int layer,const Bone* pose=nullptr,void** createdTransform=nullptr) {
        auto go=create("UnityEngine.GameObject",goCtor,{api.text(name)}); bool no=false;
        api.call(setLayer,go,{&layer}); auto tf=api.call(getTransform,go);
        if(parent) api.call(setParent,tf,{parent,&no});
        Vec3 zero{},one{1,1,1}; Quat identity;
        transforms.write(api,tf,setPosition,setRotation,pose?pose->position:zero,pose?pose->rotation:identity);
        auto scale=pose?pose->scale:one; api.call(setScale,tf,{&scale});
        if(rootTransform && parent==ref(rootTransform)) {



            auto at=std::find_if(owned.begin(),owned.end(),[&](auto handle){return ref(handle)==go;});
            if(at!=owned.end()) { retained.push_back(*at); owned.erase(at); }
        }
        if(createdTransform) *createdTransform=tf;
        return go;
    }
    std::vector<void*> descendants(void* character,const char* type) {
        bool yes=true; auto t=api.type(type); return api.elements<void*>(api.call(components,character,{t,&yes}),1024);
    }
    using Rig=std::map<std::string,void*>;
    Rig namedRig(void* character) {
        Rig rig; std::map<std::string,int> priorities;
        for(auto tf:descendants(character,"UnityEngine.Transform")) {
            if(!live(tf)) continue;
            auto raw=api.name(api.call(getName,tf));
            if(raw.starts_with("FTREZ.")) continue;
            const auto key=canonical(raw); const int priority=boneNamePriority(raw);
            if(priority>priorities[key]) { rig[key]=tf; priorities[key]=priority; }
        }
        if(!rig.count("spine1") && rig.count("spine")) rig["spine1"]=rig["spine"];
        return rig;
    }
    Matrix transformMatrix(void* tf,bool inverse=false) {
        return transforms.read(api,tf,inverse?worldToLocal:localToWorld,inverse);
    }
    glm::vec3 worldPosition(void* tf) { return glm::vec3(matrix(transformMatrix(tf))[3]); }
    bool active(void* tf) {
        return live(tf) && api.value<bool>(activeHierarchy,api.call(componentGO,tf));
    }
    void dropPreview() {
        if(previewUI) api.gchandle_free(previewUI);
        previewUI=0;
    }
    void* selectPreview() {
        if(!game.profileClass || !game.binderField || !game.mainBone)
            throw std::runtime_error("Player profile binding unavailable for this game version");
        auto ui=api.hold(modelHooks.profileObject());
        if(!active(ui) || !game.ownProfile(ui)) { dropPreview(); return nullptr; }
        if(ref(previewUI)!=ui) {
            dropPreview();
            previewUI=api.gchandle_new(ui,false);
            if(!previewUI) throw std::runtime_error("Cannot retain player profile");
        }
        auto bone=game.previewBone(api,ui);


        for(int depth=0;live(bone) && depth<32;++depth) {
            auto animator=api.call(getComponent,bone,{api.type("UnityEngine.Animator")});
            if(live(animator)) return active(bone)?bone:nullptr;
            bone=api.call(getParent,bone);
        }
        return nullptr;
    }
    void* selectTarget(uintptr_t character,bool matchActive,int frame) {
        void* target=reinterpret_cast<void*>(character);
        if(rabbit()) return selectRabbit(target,matchActive,seconds());
        if(instance==2) return selectPreview();
        if(!target && !matchActive && modelHooks.lobbyAllowed(frame)) {

            for(auto path:{"LoginSceneRoot_TSG(Clone)/Login_Ani_1/player1","Login_Ani_1/player1"}) {
                auto go=api.call(find,nullptr,{api.text(path)});
                if(live(go)) return api.call(getTransform,go);
            }
        }
        return target;
    }
    std::vector<void*> hostRenderers(void* character) {
        auto result=descendants(character,"UnityEngine.SkinnedMeshRenderer");


        std::set<void*> rig;
        for(auto tf:descendants(character,"UnityEngine.Transform")) {
            if(live(tf) && !api.name(api.call(getName,tf)).starts_with("FTREZ.")) rig.insert(tf);
        }
        std::set<void*> seen(result.begin(),result.end());
        bool includeInactive=true;
        for(auto renderer:api.elements<void*>(api.call(sceneObjectsInactive,nullptr,{api.type("UnityEngine.SkinnedMeshRenderer"),&includeInactive}),32768)) {
            if(!live(renderer) || seen.count(renderer)) continue;
            if(api.name(api.call(getName,api.call(componentGO,renderer))).starts_with("FTREZ.")) continue;
            const auto bones=api.elements<void*>(api.call(smrBones,renderer),512);
            if(std::any_of(bones.begin(),bones.end(),[&](auto bone) { return bone && bone!=character && rig.count(bone); })) {
                seen.insert(renderer); result.push_back(renderer);
            }
        }
        return result;
    }
    void dropRabbit() {
        if(rabbitTarget) api.gchandle_free(rabbitTarget);
        rabbitTarget=0; nextRabbitSearch=0;
    }
    void* selectRabbit(void* player,bool match,double clock) {
        if(!match || !active(player)) { dropRabbit(); return nullptr; }
        auto current=ref(rabbitTarget);
        if(!active(current)) { dropRabbit(); current=nullptr; }
        if(clock<nextRabbitSearch) return current;
        const auto playerRig=namedRig(player);
        auto position=[&](const Rig& rig,const char* key,glm::vec3 fallback) {
            const auto at=rig.find(key); return at==rig.end()?fallback:worldPosition(at->second);
        };
        const auto playerPosition=worldPosition(player);
        const auto pelvis=position(playerRig,"pelvis",playerPosition);
        float height=glm::length(position(playerRig,"head",playerPosition)-
            .5f*(position(playerRig,"lfoot",playerPosition)+position(playerRig,"rfoot",playerPosition)));
        if(!std::isfinite(height) || height<=0) height=4.f;
        auto cameraPosition=playerPosition;
        if(mainCamera) {
            auto camera=api.call(mainCamera);
            if(live(camera)) cameraPosition=worldPosition(api.call(getTransform,api.call(componentGO,camera)));
        }
        auto distance=[&](void* tf,const Rig& rig) {
            float closest=std::numeric_limits<float>::infinity();
            for(auto p:{worldPosition(tf),position(rig,"pelvis",worldPosition(tf))})
                for(auto reference:{playerPosition,pelvis,cameraPosition}) closest=std::min(closest,glm::length(p-reference));
            return closest/height;
        };
        const float currentDistance=current?distance(current,namedRig(current)):std::numeric_limits<float>::infinity();
        nextRabbitSearch=clock+(current?.5:.1);
        if(keepRabbit(currentDistance)) return current;
        float closest=rabbitSearchLimit(currentDistance);
        void* chosen=current; std::set<void*> visited;
        for(auto renderer:api.elements<void*>(api.call(sceneObjects,nullptr,{api.type("UnityEngine.Renderer")}),32768)) {
            if(!live(renderer)) continue;
            auto go=api.call(componentGO,renderer);
            const auto name=api.name(api.call(getName,go));
            if(name.starts_with("FTREZ.") || !rabbitName(name)) continue;
            auto tf=api.call(getTransform,go); void* candidate=nullptr;
            for(int depth=0;depth<6 && live(tf);++depth) {
                if(rabbitName(api.name(api.call(getName,tf)),true)) candidate=tf;
                else if(candidate) break;
                tf=api.call(getParent,tf);
            }
            if(!candidate || candidate==current || candidate==player || !visited.insert(candidate).second || !active(candidate)) continue;
            const auto rig=namedRig(candidate);
            bool complete=true;
            for(auto key:{"pelvis","spine1","head","lupperarm","rupperarm","lforearm","rforearm","lhand","rhand"})
                complete &= rig.count(key)!=0;
            if(!complete) continue;
            const float d=distance(candidate,rig);
            if(d<closest) { closest=d; chosen=candidate; }
        }
        if(chosen!=current) {
            auto handle=api.gchandle_new(chosen,false);
            if(!handle) throw std::runtime_error("Cannot retain rabbit target");
            dropRabbit(); rabbitTarget=handle; nextRabbitSearch=clock+.5;
            __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","Rabbit selected: %s distance=%.2f player heights",api.name(api.call(getName,chosen)).c_str(),closest);
        }
        return chosen;
    }

    bool restoreHost() {
        bool ok=true;
        try { auto go=ref(root); if(live(go)) { bool no=false; api.call(setActive,go,{&no}); } }
        catch(...) { ok=false; }
        for(auto& a:attachments) if(a.position.changed) {
            try {
                auto tf=ref(a.transform);
                if(live(tf) && api.call(getParent,tf)==ref(a.parent) && samePosition(api.value<Vec3>(getPosition,tf),a.position.last))
                    api.call(setPosition,tf,{&a.position.original});
                a.position.changed=false;
            } catch(...) { ok=false; }
        }
        for(auto& s:original) if(s.changed) {
            try { restoreVisibility(s,[&](bool enabled) { auto obj=ref(s.handle); if(live(obj)) api.call(setEnabled,obj,{&enabled}); }); }
            catch(...) { ok=false; }
        }
        for(auto& s:animators) if(s.changed) {
            try {
                auto obj=ref(s.handle);
                if(live(obj) && api.value<int>(getCulling,obj)==0) api.call(setCulling,obj,{&s.culling});
                s.changed=false;
            } catch(...) { ok=false; }
        }
        return ok;
    }
    void releaseHost() {
        const auto handles=hostRetained;
        for(auto handle:handles) release(handle);
        host=0; hostBones.clear(); rest.clear(); original.clear(); animators.clear();
        attachments.clear(); skeletonObjects.clear(); topology.clear(); matched=0;
        nextTopology=0; rejectedPoses=0; dynamics.reset(); lastPoseTime=0;
    }
    bool parkSnapshot() {
        if(snapshotParked) return true;
        if(rabbit() || !asset || !buildQueue.done() || !live(ref(carrier)) || !live(ref(root))) return false;
        if(!restoreHost()) throw std::runtime_error("Snapshot original restore pending");
        bool no=false;
        api.call(setParent,ref(carrierTransform),{nullptr,&no});
        api.call(dontDestroy,nullptr,{ref(carrier)}); carrierLinked=false;
        releaseHost(); snapshotParked=true;
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
            "saved instance=%zu model=%d bones=%zu meshes=%zu textures=%zu",instance,selected,boneRefs.size(),parts.size(),textures.size());
        return true;
    }
    void followHost(const glm::mat4& world) {
        if(!carrierTransform) return;
        auto tf=ref(carrierTransform); bool no=false;
        if(auto placement=snapshotTransform(world)) {
            if(carrierLinked) {
                api.call(setParent,tf,{nullptr,&no});
                api.call(dontDestroy,nullptr,{ref(carrier)}); carrierLinked=false;
            }
            transforms.write(api,tf,setPosition,setRotation,placement->position,placement->rotation);
            api.call(setScale,tf,{&placement->scale});
        } else {
            if(!carrierLinked) {
                api.call(setParent,tf,{ref(host),&no}); carrierLinked=true;
                __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
                    "parent_fallback instance=%zu model=%d reason=non_trs_host",instance,selected);
            }
            Vec3 zero{},one{1,1,1}; Quat identity;
            transforms.write(api,tf,setPosition,setRotation,zero,identity);
            api.call(setScale,tf,{&one});
        }
    }
    void restoreSnapshot(void* character) {
        const double start=seconds();
        host=retainHost(character);
        buildTask({ModelBuildPhase::Capture});
        buildTask({ModelBuildPhase::Rig});
        int layer=api.value<int>(getLayer,api.call(componentGO,character));
        if(layer!=buildLayer) {
            api.call(setLayer,ref(carrier),{&layer});
            for(auto tf:descendants(ref(rootTransform),"UnityEngine.Transform"))
                api.call(setLayer,api.call(componentGO,tf),{&layer});
            buildLayer=layer;
        }
        validateBuiltHost(); activateBuilt(); snapshotParked=false;
        firstPerson=false;
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
            "restored instance=%zu model=%d rebind_ms=%.3f texture_uploads=0 mesh_builds=0",
            instance,selected,(seconds()-start)*1000.);
    }
    bool cleanup() {
        restorePending=true;
        bool ok=restoreHost();
        for(auto it=owned.rbegin();it!=owned.rend();++it) if(*it) {
            try { auto obj=ref(*it); if(live(obj)) api.call(destroy,nullptr,{obj}); api.gchandle_free(*it); *it=0; }
            catch(...) { ok=false; }
        }
        if(!ok) return false;
        for(auto h:retained) api.gchandle_free(h);
        owned.clear(); retained.clear(); hostRetained.clear(); original.clear(); animators.clear(); boneRefs.clear(); hostBones.clear(); rest.clear(); parts.clear();
        if(carrier) __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT","released instance=%zu model=%d",instance,selected);
        carrier=carrierTransform=0; snapshotParked=carrierLinked=false;
        customShaderHandle=customMaterialHandle=0; customError.clear();
        textures.clear(); styleObjects.clear(); shaderDescription.clear(); styleNotice.clear(); appliedStyle=appliedHairTone=-1;
        surfaceWorker.reset();
        boneUpdates.clear();
        dynamics.clear(); surfaceCollision.clear(); lastPoseTime=0; nextStyleProbe=0; rabbitBaseline=0;
        armAvoidance.clear();
        rejectedPoses=0; appliedFirstPerson=false;
        topology.clear(); nextTopology=0; firstPerson=false; headIndex=neckIndex=-1;
        textureRequest.reset();
        buildQueue.clear(); buildBoneArray=buildBindArray=0; buildStarted=buildCpuMs=0; buildPhase=nullptr;
        perf={};
        attachments.clear(); skeletonObjects.clear();
        host=root=rootTransform=0; selected=-1; asset.reset(); shared.reset(); sharedModels().trim();
        sharedStyleGeneration=0; matched=0; restorePending=false; return true;
    }
    bool discardPending() {
        if(!pending) return true;
        bool clean=false;
        {
            ModelTemps temps{pending->api};
            pending->dropRabbit(); pending->dropPreview();
            clean=pending->cleanup();
        }
        if(clean) pending.reset();
        return clean;
    }
    bool has(void* material,const char* key) { return api.value<bool>(hasProperty,material,{api.text(key)}); }
    void number(void* material,const char* key,float value) {
        if(has(material,key)) api.call(setFloat,material,{api.text(key),&value});
    }
    void color(void* material,const char* key,std::array<float,4> value) {
        if(has(material,key)) api.call(setNamedColor,material,{api.text(key),value.data()});
    }
    void texture(void* material,const char* key,void* value) {
        if(has(material,key)) api.call(setNamedTexture,material,{api.text(key),value});
    }
    bool usableShader(void* shader) { return live(shader) && api.value<bool>(supported,shader); }
    void* shader(const std::string& name) {
        auto result=api.call(shaderFind,nullptr,{api.text(name)});
        if(usableShader(result)) return result;
        for(auto object:api.elements<void*>(api.call(findObjects,nullptr,{api.type("UnityEngine.Shader")}),32768))
            if(live(object) && api.name(api.call(getName,object))==name && usableShader(object)) return object;


        if(loadedBundles && bundleNames && loadAsset) {
            auto leaf=name.substr(name.find_last_of('/')+1)+".shader";
            for(auto bundle:api.elements<void*>(api.call(loadedBundles),16384)) if(live(bundle)) {
                for(auto item:api.elements<void*>(api.call(bundleNames,bundle),65536)) {
                    auto path=api.name(item);
                    if(path.size()<leaf.size() || path.compare(path.size()-leaf.size(),leaf.size(),leaf)) continue;
                    auto object=api.call(loadAsset,bundle,{item,api.type("UnityEngine.Shader")});
                    if(usableShader(object) && api.name(api.call(getName,object))==name) return object;
                }
            }
        }
        return nullptr;
    }
    void* materialTemplate(bool hostOnly=false) {
        auto cached=ref(shared->hostMaterial);
        if(live(cached) && usableShader(api.call(getShader,cached)) &&
           (!lobbyHost || shared->hostMaterialLobby)) return cached;
        void* best=nullptr; int bestScore=0;
        auto consider=[&](void* m,bool hostMaterial) {
            if(!live(m)) return;
            auto sh=api.call(getShader,m); if(!usableShader(sh)) return;
            auto name=api.name(api.call(getName,sh));
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
            if(name.find("outline")!=std::string::npos || name.find("unlit")!=std::string::npos ||
               name.find("cartoon")!=std::string::npos || name.find("terrain")!=std::string::npos ||
               name.find("particle")!=std::string::npos || name.find("ui/")!=std::string::npos) return;
            if(!(has(m,"_BaseMap")||has(m,"_MainTex")) || !(has(m,"_BaseColor")||has(m,"_Color"))) return;
            int score=hostMaterial?100:0;
            if(name.find("pbr")!=std::string::npos) score+=30;
            if(name.find("standard")!=std::string::npos || name.find("lit")!=std::string::npos) score+=20;
            if(score>bestScore) { best=m; bestScore=score; }
        };
        for(auto& saved:original) for(auto m:api.elements<void*>(api.call(getMaterials,ref(saved.handle)),128)) consider(m,true);
        if(!best && !hostOnly) for(auto m:api.elements<void*>(api.call(findObjects,nullptr,{api.type("UnityEngine.Material")}),32768)) consider(m,false);
        if(!best) return nullptr;
        // New styles selected after leaving the lobby must start from the same
        // retained lobby template, not a new match host's material defaults.
        auto snapshot=create("UnityEngine.Material",matClone,{best});
        shared->hostMaterial=shareOwned(snapshot); shared->hostMaterialLobby=lobbyHost;
        return snapshot;
    }
    uint32_t customShaderHandle=0,customMaterialHandle=0;
    std::string customError;
    void* folderShader() {
        customShaderHandle=shared->shader; customMaterialHandle=shared->material;


        if(customShaderHandle) {
            auto cached=ref(customShaderHandle);
            if(usableShader(cached)) { customError.clear(); return cached; }
            if(live(cached)) {
                customError="Folder Shader unsupported by current render pipeline";
                return nullptr;
            }
            shared->shader=shared->material=0;
            customShaderHandle=customMaterialHandle=0;
        }
        customError=asset->rendererError;
        if(asset->rendererBytes.empty()) return nullptr;
        void* bundle=nullptr;
        const auto ownedBegin=owned.size();
        auto unload=api.method("UnityEngine.AssetBundle","Unload",{"System.Boolean"},"System.Void",false);
        try {
            auto load=api.method("UnityEngine.AssetBundle","LoadFromMemory",{"System.Byte[]"},"UnityEngine.AssetBundle",true);
            auto bytes=api.array<uint8_t>("System.Byte",std::span<const uint8_t>(asset->rendererBytes));
            bundle=api.call(load,nullptr,{bytes});
            if(!live(bundle)) throw std::runtime_error("AssetBundle load failed or conflicting loaded bundle");
            std::string path=asset->rendererShader;
            if(path.empty()) {
                for(auto item:api.elements<void*>(api.call(bundleNames,bundle),65536)) {
                    auto candidate=api.name(item);
                    if(candidate.size()<7 || candidate.substr(candidate.size()-7)!=".shader") continue;
                    if(!path.empty()) throw std::runtime_error("Multiple shaders: specify renderer.shader in model.json");
                    path=candidate;
                }
            }
            if(path.empty()) throw std::runtime_error("No Shader asset in bundle");
            auto result=api.call(loadAsset,bundle,{api.text(path),api.type("UnityEngine.Shader")});
            if(!usableShader(result)) throw std::runtime_error("Shader missing or unsupported by game graphics API");



            auto loadTextures=api.method("UnityEngine.AssetBundle","LoadAllAssets",{"System.Type"},"UnityEngine.Object[]",false);
            auto bundledTextures=api.elements<void*>(api.call(loadTextures,bundle,{api.type("UnityEngine.Texture")}),32);
            for(auto texture:bundledTextures) if(live(texture)) retain(texture,true);
            customShaderHandle=retain(result,true);
            if(!asset->rendererMaterial.empty()) {
                auto material=api.call(loadAsset,bundle,{api.text(asset->rendererMaterial),api.type("UnityEngine.Material")});
                if(!live(material) || api.call(getShader,material)!=result)
                    throw std::runtime_error("Renderer material missing or uses a different Shader");
                customMaterialHandle=retain(material,true);
            }


            bool all=false; api.call(unload,bundle,{&all}); bundle=nullptr;
            shareOwnedFrom(ownedBegin);
            shared->shader=customShaderHandle; shared->material=customMaterialHandle;
            customError.clear();
            return result;
        } catch(const std::exception& e) {
            customError=e.what();
            if(bundle) { try { bool all=false; api.call(unload,bundle,{&all}); } catch(...) {} }
            for(size_t i=owned.size();i>ownedBegin;--i) {
                auto h=owned[i-1];
                try { auto obj=ref(h); if(live(obj)) api.call(destroy,nullptr,{obj}); } catch(...) {}
                api.gchandle_free(h);
            }
            owned.resize(ownedBegin); customShaderHandle=customMaterialHandle=0;
            return nullptr;
        }
    }
    void* importedToon(bool gltf=false) {
#ifdef FT_MODEL_TOON_BUNDLE

        static uint32_t bundleHandle=0,shaderHandles[2]{};
        auto& shaderHandle=shaderHandles[gltf?1:0];
        if(shaderHandle && usableShader(ref(shaderHandle))) return ref(shaderHandle);
        try {
            auto bundle=bundleHandle?api.gchandle_get_target(bundleHandle):nullptr;
            if(!live(bundle)) {
                if(bundleHandle) { api.gchandle_free(bundleHandle); bundleHandle=0; }
                auto load=api.method("UnityEngine.AssetBundle","LoadFromMemory",{"System.Byte[]"},"UnityEngine.AssetBundle",true);
                auto bytes=api.array<uint8_t>("System.Byte",std::span<const uint8_t>(ft_model_toon_start,ft_model_toon_end-ft_model_toon_start));
                bundle=api.call(load,nullptr,{bytes});
                if(!live(bundle)) return nullptr;
                bundleHandle=api.gchandle_new(bundle,false);
            }
            auto result=api.call(loadAsset,bundle,{api.text(gltf?"assets/ftrezgltf/gltf.shader":"assets/ftreztoon/simpleurptoonlitoutlineexample.shader"),api.type("UnityEngine.Shader")});
            if(usableShader(result)) {
                shaderHandle=api.gchandle_new(result,false);
                auto unload=api.method("UnityEngine.AssetBundle","Unload",{"System.Boolean"},"System.Void",false);
                bool all=false; api.call(unload,bundle,{&all});
                api.gchandle_free(bundleHandle); bundleHandle=0;
                return result;
            }
        } catch(const std::exception& e) {
            __android_log_print(ANDROID_LOG_WARN,"FTREZ-MODEL","EeveeToon unavailable: %s",e.what());
        }
#endif
        return nullptr;
    }
    void applySharedStyle(const SharedModelResources::Style& value,int style,int hairTone) {
        for(size_t i=0;i<parts.size();++i) {
            std::vector<void*> list;
            for(auto handle:value.materials.at(i)) list.push_back(ref(handle));
            api.call(setMaterials,ref(parts[i].renderer),{api.refs("UnityEngine.Material",list)});
        }
        releaseLocalStyle();
        customShaderHandle=shared->shader; customMaterialHandle=shared->material;
        appliedStyle=style; appliedHairTone=hairTone;
        shaderDescription=value.shader; styleNotice=value.notice; sharedStyleGeneration=value.generation;
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SHARED",
            "materials instance=%zu model=%d resource=%p generation=%llu lobby=%d",instance,selected,shared.get(),
            static_cast<unsigned long long>(value.generation),value.lobby);
    }
    void restyle(int style,int hairTone) {
        const auto key=std::make_pair(style,hairTone);
        auto found=shared->styles.find(key);
        bool cachedValid=false;
        if(found!=shared->styles.end() && (!(lobbyHost || shared->hostMaterialLobby) || found->second.lobby)) {
            bool valid=found->second.materials.size()==parts.size();
            for(const auto& group:found->second.materials) for(auto h:group)
                valid=valid && live(ref(h)) && usableShader(api.call(getShader,ref(h)));
            cachedValid=valid;
            if(valid && (found->second.notice.empty() || seconds()<found->second.retryAfter)) {
                applySharedStyle(found->second,style,hairTone); return;
            }
        }
        void* source=nullptr; void* baseShader=nullptr;
        if(style==3) baseShader=importedToon();
        if(style==4) baseShader=folderShader();
        const bool custom=style==4 && baseShader;
        const bool gltf=asset->gltfMaterials && !custom && (style==5 || style==4);
        if(gltf) { baseShader=importedToon(true); if(!baseShader) throw std::runtime_error("GLB material shader unavailable"); }
        const bool eevee=baseShader && api.name(api.call(getName,baseShader))=="FTREZ/EeveeToon";
        if((style==2 || style==3 || style==4) && !baseShader) baseShader=shader("unity-compatible/fast-cartoon");
        const bool toon=!gltf && baseShader!=nullptr;
        if(!baseShader) { source=materialTemplate(); if(source) baseShader=api.call(getShader,source); }
        if(!baseShader) for(auto name:{"Universal Render Pipeline/Simple Lit","Universal Render Pipeline/Lit","Standard",
                                     "Universal Render Pipeline/Unlit","Unlit/Texture"}) {
            baseShader=shader(name); if(baseShader) break;
        }
        if(!baseShader) throw std::runtime_error("No compatible character shader is loaded");
        auto outlineShader=(style>0 && style<4 && !custom)?shader("visual-fx/pbr-std-outline-only"):nullptr;


        std::string notice;
        if(gltf && asset->rendererEmbedded && !asset->rendererError.empty()) notice="; Embedded renderer fallback: "+asset->rendererError;
        if(style==4 && !custom) notice="; Custom renderer fallback: "+customError;
        if(style==3 && !eevee) notice="; EeveeToon bundle unavailable";
        if((style==2 || style==3 || style==4) && !toon && !gltf) notice+="; toon unavailable, fallback shader";
        if(style>0 && style<4 && !custom && !outlineShader) notice+="; outline unavailable";
        auto name=api.name(api.call(getName,baseShader));
        // Probe late shader availability without allocating identical fallback
        // materials every five seconds. Every consumer shares this result too.
        if(cachedValid &&
           found->second.notice==notice && found->second.shader==name &&
           !found->second.materials.empty() && !found->second.materials[0].empty()) {
            auto material=ref(found->second.materials[0][0]);
            if(live(material) && api.call(getShader,material)==baseShader) {
                found->second.retryAfter=seconds()+5;
                applySharedStyle(found->second,style,hairTone); return;
            }
        }
        const auto begin=owned.size();
        std::vector<void*> base,alpha,outline;
        for(size_t i=0;i<asset->materials.size();++i) {
            const auto& m=asset->materials[i];
            auto materialTemplate=custom?ref(customMaterialHandle):source;
            auto mat=create("UnityEngine.Material",materialTemplate?matClone:matCtor,{materialTemplate?materialTemplate:baseShader});

            if(source) for(auto key:{"_BumpMap","_NormalMap","_MaskTex","_MaskMap","_MetallicGlossMap","_SpecGlossMap","_OcclusionMap","_EmissionMap"}) texture(mat,key,nullptr);
            texture(mat,"_BaseMap",ref(textures[i])); texture(mat,"_MainTex",ref(textures[i]));
            const auto tint=materialColor(m,hairTone);
            color(mat,"_BaseColor",tint); color(mat,"_Color",tint);
            // A folder renderer owns its lighting/emission defaults, including Eevee.
            // Only built-in styles receive the runtime's generic material preset.
            if(!custom && !gltf) applyStyle(toon?2:0,source!=nullptr,asset->lighting,
                [&](auto key,float v){number(mat,key,v);},[&](auto key,auto v){color(mat,key,v);},
                [&](auto key){api.call(enableKeyword,mat,{api.text(key)});},[&](auto key){texture(mat,key,nullptr);});
            if(eevee && !custom) {
                auto label=m.name;
                std::transform(label.begin(),label.end(),label.begin(),[](unsigned char c){return char(std::tolower(c));});

                auto semantic=label.substr(0,label.find('.'));
                const bool soyoFacial=semantic=="soyo_material";
                const auto separator=semantic.find_last_of('_');
                if(separator!=std::string::npos) semantic=semantic.substr(separator+1);
                const bool face=semantic=="face" || semantic=="facial" || semantic=="eye" || semantic=="eyes" || soyoFacial;
                number(mat,"_IsFace",face?1.f:0.f);
                number(mat,"_BandLow",.32f); number(mat,"_BandHigh",.64f);
                number(mat,"_BandSoftness",face?.018f:.008f);
                number(mat,"_ReceiveShadowMappingAmount",face?.65f:1.f);
                number(mat,"_DirectLightMultiplier",.9f); number(mat,"_IndirectLightMultiplier",.6f);
                number(mat,"_UseEmission",0.f); number(mat,"_UseOcclusion",0.f);
                color(mat,"_IndirectLightMinColor",{.08f,.08f,.08f,1.f});
            }
            if(gltf) applyGltfMaterial(m,
                [&](const char* key,float value) { number(mat,key,value); },
                [&](const char* key,std::array<float,4> value) { api.call(setVector,mat,{api.text(key),&value}); },
                [&](const char* key,int index) { if(index>=0) texture(mat,key,ref(textures.at(asset->materials.size()+size_t(index)))); },
                [&](int queue) { api.call(setQueue,mat,{&queue}); });
            if(custom) for(const auto& property:m.shaderFloats)
                number(mat,property.first.c_str(),property.second);
            if(m.flags&materialDoubleSided) {
                number(mat,"_Cull",0.f); number(mat,"_CullMode",0.f);
            }
            base.push_back(mat);
            void* cutout=gltf?mat:nullptr;
            if(!gltf && std::any_of(asset->parts.begin(),asset->parts.end(),[&](const Part& p){return p.material==i && alphaPart(p);})) {
                cutout=create("UnityEngine.Material",matClone,{mat});
                api.call(enableKeyword,cutout,{api.text("_ALPHATEST_ON")});
                if(eevee) api.call(enableKeyword,cutout,{api.text("_UseAlphaClipping")});
                number(cutout,"_AlphaClip",1); number(cutout,"_Cutoff",.5f);
            }
            alpha.push_back(cutout);
            void* line=nullptr;
            if(outlineShader) {
                line=create("UnityEngine.Material",matCtor,{outlineShader});
                color(line,"_LineColor",outlineColor(m)); number(line,"_OutlineWidth",.003f); number(line,"_OutlineCull",1);
                api.call(enableKeyword,line,{api.text("_MODE_DEFAULT")});
            }
            outline.push_back(line);
        }

        SharedModelResources::Style published;
        std::map<void*,uint32_t> handles;
        for(auto it=owned.begin()+begin;it!=owned.end();++it) handles.emplace(ref(*it),*it);
        for(size_t i=0;i<parts.size();++i) {
            const auto& p=asset->parts[i]; std::vector<void*> list{alphaPart(p)?alpha[p.material]:base[p.material]};
            if(outlineShader && outlinePart(p,style)) list.push_back(outline[p.material]);
            api.call(setMaterials,ref(parts[i].renderer),{api.refs("UnityEngine.Material",list)});
            std::vector<uint32_t> group; for(auto material:list) group.push_back(handles.at(material));
            published.materials.push_back(std::move(group));
        }
        published.shader=name; published.notice=notice; published.lobby=lobbyHost || shared->hostMaterialLobby;
        published.generation=++shared->generation; published.retryAfter=seconds()+5;
        shareOwnedFrom(begin);
        shared->styles[key]=std::move(published);
        sharedStyleGeneration=shared->styles.at(key).generation;
        releaseLocalStyle();
        appliedStyle=style; appliedHairTone=hairTone; shaderDescription=std::move(name); styleNotice=std::move(notice);
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","Style=%s shader=%s toon=%d outline=%d parts=%zu%s",styleName(style),shaderDescription.c_str(),toon,outlineShader!=nullptr,parts.size(),styleNotice.c_str());
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SHARED","publish model=%d instance=%zu resource=%p generation=%llu lobby=%d",selected,instance,shared.get(),static_cast<unsigned long long>(sharedStyleGeneration),lobbyHost);
    }
    void mapSkeleton(void* character,const std::vector<void*>& renderers) {
        auto rootInverse=matrix(transformMatrix(character,true));
        HostSkeleton candidates;
        std::map<void*,Matrix> calibrated;
        for(auto renderer:renderers) {
            if(!live(renderer)) continue;
            auto go=api.call(componentGO,renderer);
            if(api.name(api.call(getName,go)).starts_with("FTREZ.")) continue;

            auto mesh=api.call(smrMesh,renderer); if(!live(mesh)) continue;
            auto bones=api.elements<void*>(api.call(smrBones,renderer),512);
            auto binds=api.elements<Matrix>(api.call(meshBind,mesh),512);
            if(bones.size()!=binds.size()) continue;
            auto tf=api.call(getTransform,go);
            auto renderToRoot=rootInverse*matrix(transformMatrix(tf));
            for(size_t i=0;i<bones.size();++i) {
                if(!live(bones[i])) continue;


                auto ancestor=bones[i];
                for(int depth=0;live(ancestor) && depth<64;++depth) {
                    if(!skeletonObjects.insert(reinterpret_cast<uintptr_t>(ancestor)).second || ancestor==character) break;
                    ancestor=api.call(getParent,ancestor);
                }
                auto raw=api.name(api.call(getName,bones[i]));
                auto key=canonical(raw);
                auto bind=matrix(binds[i]);
                if(!std::isfinite(glm::determinant(bind)) || std::abs(glm::determinant(bind))<1e-8f) continue;
                auto m=renderToRoot*glm::inverse(bind); (void)rotation(m);
                calibrated[bones[i]]=stored(m);
                candidates.offer(key,reinterpret_cast<uintptr_t>(bones[i]),stored(m),boneNamePriority(raw));
            }
        }
        if(isHuman && humanBone) for(auto animator:descendants(character,"UnityEngine.Animator")) {
            if(!live(animator) || !api.value<bool>(isHuman,animator)) continue;
            for(int slot=0;slot<55;++slot) {
                auto bone=api.call(humanBone,animator,{&slot}); auto found=calibrated.find(bone);
                if(found==calibrated.end()) continue;
                candidates.offer(humanBoneName(slot),reinterpret_cast<uintptr_t>(bone),found->second,3);
            }
        }
        rest.resize(asset->bones.size()); hostBones.resize(asset->bones.size());
        for(size_t i=0;i<asset->bones.size();++i) {
            auto found=candidates.get(boneKey(asset->bones[i]));
            if(!found && rabbit() && boneKey(asset->bones[i])=="spine1") found=candidates.get("spine");
            if(!found) continue;
            hostBones[i]=retainHost(reinterpret_cast<void*>(found->object)); rest[i]=found->bind; ++matched;
        }
        auto requireBone=[&](const char* key) {
            for(size_t i=0;i<asset->bones.size();++i) if(boneKey(asset->bones[i])==key && hostBones[i]) return i;
            __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","Skeleton unresolved=%s matched=%zu calibrated=%zu",key,matched,calibrated.size());
            for(auto bone:descendants(character,"UnityEngine.Transform")) {
                auto raw=api.name(api.call(getName,bone));
                if(canonical(raw)!=key) continue;
                auto parent=api.call(getParent,bone);
                __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","Hierarchy key=%s object=%p raw=%s calibrated=%d parent=%s",key,bone,raw.c_str(),int(calibrated.count(bone)),parent?api.name(api.call(getName,parent)).c_str():"<none>");
            }
            throw std::runtime_error(std::string("Waiting for calibrated bone: ")+key);
        };
        auto hips=requireBone("pelvis"),head=requireBone("head");
        requireBone("lupperarm"); requireBone("rupperarm");
        if(rabbit()) {
            requireBone("lforearm"); requireBone("rforearm"); requireBone("lhand"); requireBone("rhand");
        }
        if(matched<(rabbit()?9u:12u)) throw std::runtime_error("Incomplete character skeleton");
        float targetHeight=0;
        if(!rabbit()) {
            requireBone("lthigh"); requireBone("rthigh"); requireBone("lcalf"); requireBone("rcalf");
            requireBone("lfoot"); requireBone("rfoot");
        } else {
            rabbitBaseline=glm::length(worldPosition(ref(hostBones[head]))-worldPosition(ref(hostBones[hips])));

            struct Bounds { Vec3 center,extents; };
            float low=std::numeric_limits<float>::infinity(),high=-low;
            for(auto renderer:descendants(character,"UnityEngine.Renderer")) if(live(renderer)) {
                if(api.name(api.call(getName,api.call(componentGO,renderer))).starts_with("FTREZ.")) continue;
                const auto b=api.value<Bounds>(getBounds,renderer);
                if(!finite(vec(b.center)) || !finite(vec(b.extents))) continue;
                low=std::min(low,b.center.y-b.extents.y); high=std::max(high,b.center.y+b.extents.y);
            }
            const float height=high-low;
            if(std::isfinite(height) && height>.05f && height<5.f)
                targetHeight=height/glm::length(glm::vec3(glm::inverse(rootInverse)[1]));
        }

        const auto hostPositions=humanPositions(*asset,&rest);
        modelPositions=humanPositions(*asset);
        modelLengths=bodyLengths(modelPositions,hostPositions); hostLengths=bodyLengths(hostPositions,modelPositions);
        const auto extent=modelExtent(*asset);
        bodyHeight=extent.height();
        headRadius=std::max(extent.top-asset->bones[head].position.y,0.f)*.5f;
        fit=modelFit(modelLengths,hostLengths,bodyHeight,targetHeight);
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL","%s dimensions: visual=%.3f hostChains=%.3f target=%.3f fit=%.3f",instance==1?"Rabbit":instance==2?"Preview":"Self",bodyHeight,hostLengths.torso+hostLengths.legs,targetHeight,fit);
    }
    Topology hostTopology(const std::vector<void*>& renderers) {
        std::set<void*> ourRenderers;
        for(const auto& p:parts) ourRenderers.insert(ref(p.renderer));
        Topology result;
        for(auto renderer:renderers) {
            if(!live(renderer) || ourRenderers.count(renderer)) continue;
            if(api.name(api.call(getName,api.call(componentGO,renderer))).starts_with("FTREZ.part.")) continue;
            auto mesh=api.call(smrMesh,renderer);

            result.emplace_back(reinterpret_cast<uintptr_t>(renderer),reinterpret_cast<uintptr_t>(mesh));
        }
        std::sort(result.begin(),result.end()); return result;
    }
    void findAttachments(void* character) {
        std::map<void*,size_t> anchors;
        for(size_t i=0;i<hostBones.size();++i) if(hostBones[i]) anchors.emplace(ref(hostBones[i]),i);
        std::set<void*> seen;
        for(const auto& a:attachments) seen.insert(ref(a.transform));
        for(auto renderer:descendants(character,"UnityEngine.MeshRenderer")) {
            if(attachments.size()>=128) break;
            if(!live(renderer) || !api.value<bool>(getEnabled,renderer)) continue;
            auto go=api.call(componentGO,renderer);
            if(!api.value<bool>(activeHierarchy,go)) continue;
            auto name=api.name(api.call(getName,go));
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
            if(name.starts_with("ftrez.") || name.find("shadow")!=std::string::npos) continue;
            auto tf=api.call(getTransform,go);


            for(int depth=0;live(tf) && tf!=character && depth<64;++depth) {
                if(skeletonObjects.count(reinterpret_cast<uintptr_t>(tf))) break;
                auto parent=api.call(getParent,tf); auto anchor=anchors.find(parent);
                if(anchor!=anchors.end()) {
                    if(seen.insert(tf).second) attachments.push_back({retainHost(tf),retainHost(parent),anchor->second,{}});
                    break;
                }
                tf=parent;
            }
        }
    }
    void updateAttachments(bool move,const std::vector<PoseBone>& pose,
                           const std::vector<std::optional<Matrix>>& now,
                           const glm::mat4& modelWorld,const glm::mat4& hostWorld) {
        for(auto it=attachments.begin();it!=attachments.end();) {
            auto& a=*it; auto tf=ref(a.transform); auto parent=ref(a.parent);
            if(!live(tf) || !live(parent) || api.call(getParent,tf)!=parent) {
                release(a.transform); release(a.parent); it=attachments.erase(it); continue;
            }
            auto current=api.value<Vec3>(getPosition,tf);
            auto baseline=a.position.baseline(current);
            if(!move || !now[a.bone]) {
                if(a.position.changed) api.call(setPosition,tf,{&baseline});
                a.position.changed=false; ++it; continue;
            }
            auto displacement=[&](size_t bone) {
                const auto source=glm::vec3(hostWorld*matrix(*now[bone])[3]);
                const auto target=glm::vec3(modelWorld*glm::vec4(vec(pose[bone].position),1));
                return target-source;
            };
            auto delta=displacement(a.bone);
            const int human=asset->bones[a.bone].human;


            if(human==0 || (human>=0 && human<13 && ((1<<human)&0x1b80))) {
                const int left=human==0?1:13,right=left+1;
                int li=-1,ri=-1;
                for(size_t i=0;i<asset->bones.size();++i) if(now[i]) {
                    if(asset->bones[i].human==left) li=int(i);
                    if(asset->bones[i].human==right) ri=int(i);
                }
                if(li>=0 && ri>=0) delta=(displacement(li)+displacement(ri))*.5f;
            }
            const auto localDelta=glm::vec3(matrix(transformMatrix(parent,true))*glm::vec4(delta,0));
            a.position.last=vec(vec(baseline)+localDelta);

            a.position.changed=true;
            api.call(setPosition,tf,{&a.position.last}); ++it;
        }
    }
    void prefetchTexture(size_t first) {
        textureRequest.reset();
        if(!textureUpload.ready()) return;
        for(size_t i=first;i<modelTextureCount(*asset);++i) {
            if(shared && shared->textures[i].has_value()) continue;
            if(modelTextureBytes(*asset,i).empty()) continue;
            if(!asset->textureSources.empty() && asset->textureSources.at(i)!=int32_t(i)) continue;
            textureRequestSource=i; textureRequest=requestModelTexture(asset,i); return;
        }
    }
    void beginBuild(void* character,std::shared_ptr<const Model> model,int index) {
        asset=std::move(model); selected=index; host=retainHost(character);
        shared=sharedModels().acquire(asset,[&] {
            __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SHARED","create model=%d origin=%zu lobby=%d",selected,instance,lobbyHost);
            return std::make_shared<SharedModelResources>(api,alive,destroy,*asset);
        });
        {
            auto& s=state(); std::lock_guard lock(s.mutex);
            if(s.slots[instance].asset==asset) s.sharedSources[selected]=asset;
        }
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SHARED","acquire model=%d instance=%zu resource=%p",selected,instance,shared.get());
        buildQueue.reset(gpu?asset->bones.size():0,modelTextureCount(*asset),asset->parts.size());
        buildStarted=seconds(); buildCpuMs=0; buildPhase=nullptr;
        prefetchTexture(0);
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD",
            "begin instance=%zu model=%d bones=%zu vertices=%zu parts=%zu materials=%zu budget_ms=2",
            instance,selected,asset->bones.size(),asset->vertices.size(),asset->parts.size(),asset->materials.size());
    }
    void buildTask(ModelBuildTask task,const DecodedModelTexture* decoded=nullptr) {
        auto character=ref(host);
        if(!live(character)) throw std::runtime_error("Build host was destroyed");
        switch(task.phase) {
        case ModelBuildPhase::Capture: {
            const auto renderers=hostRenderers(character);
            topology=hostTopology(renderers);
            for(auto renderer:renderers) if(live(renderer)) {
                if(api.name(api.call(getName,api.call(componentGO,renderer))).starts_with("FTREZ.part.")) continue;
                original.push_back({retainHost(renderer),api.value<bool>(getEnabled,renderer),false});
            }
            for(auto animator:descendants(character,"UnityEngine.Animator")) if(live(animator))
                animators.push_back({retainHost(animator),api.value<int>(getCulling,animator),false});
            if(lobbyHost) materialTemplate(true);
            break;
        }
        case ModelBuildPhase::Rig: {
            std::vector<void*> renderers; renderers.reserve(original.size());
            for(const auto& saved:original) renderers.push_back(ref(saved.handle));
            mapSkeleton(character,renderers); break;
        }
        case ModelBuildPhase::Physics:
            dynamics.prepare(*asset); surfaceCollision.prepare(*asset); armAvoidance.prepare(*asset);
            boneUpdates.prepare(*asset,[&](size_t part) { return gpu && !surfaceCollision.affects(part); });
            surfaceWorker.reset();
            if(gpu && surfaceCollision.queryWorkers()>=3) {
                try { surfaceWorker=std::make_unique<ModelCollisionWorker>(); }
                catch(const std::system_error&) {}
            }
            __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","physics instance=%zu model=%d surface=%d body_triangles=%zu surface_async_worker=%d",
                instance,selected,surfaceCollision.active(),surfaceCollision.bodyTriangles(),bool(surfaceWorker));
            break;
        case ModelBuildPhase::Root: {
            for(size_t i=0;i<asset->bones.size();++i) {
                if(asset->bones[i].human==10) headIndex=int(i);
                if(asset->bones[i].human==9) neckIndex=int(i);
            }
            if(neckIndex<0 && headIndex>=0) neckIndex=asset->bones[headIndex].parent;
            buildLayer=api.value<int>(getLayer,api.call(componentGO,character));
            void* parent=character;
            if(!rabbit()) {
                auto holder=gameObject("FTREZ.ModelSnapshot",nullptr,buildLayer);
                carrier=retain(holder); carrierTransform=retain(api.call(getTransform,holder));
                api.call(dontDestroy,nullptr,{holder}); parent=ref(carrierTransform);
            }
            auto go=gameObject(rabbit()?"FTREZ.RabbitModel":instance==2?"FTREZ.PreviewModel":"FTREZ.SelfModel",parent,buildLayer);
            root=retain(go); bool no=false; api.call(setActive,go,{&no});
            rootTransform=retain(api.call(getTransform,go)); break;
        }
        case ModelBuildPhase::Bone: {
            const auto& bone=asset->bones.at(task.index);
            void* tf=nullptr;
            gameObject("FTREZ.bone."+bone.name,ref(rootTransform),buildLayer,&bone,&tf);
            boneRefs.push_back(retain(tf)); break;
        }
        case ModelBuildPhase::Texture: {
            if(shared->textures[task.index].has_value()) {
                if(textureRequest && textureRequestSource<=task.index) prefetchTexture(task.index+1);
                textures.push_back(*shared->textures[task.index]); break;
            }
            const auto material=task.index; const auto& encoded=modelTextureBytes(*asset,material);
            auto sampling=modelTextureSampling(*asset,material);
            uint32_t handle=0;
            const int32_t source=asset->textureSources.empty()?int32_t(material):asset->textureSources.at(material);
            if(source>=0 && size_t(source)<material) handle=textures.at(size_t(source));
            else if(!encoded.empty()) {
                if(source!=int32_t(material)) throw std::runtime_error("Invalid model texture plan");
                void* texture=nullptr;
                if(decoded && decoded->pixels) {
                    texture=textureUpload.upload(api,[&](void* ctor,std::initializer_list<void*> args) {
                        return create("UnityEngine.Texture2D",ctor,args);
                    },*decoded,sampling);
                } else {
                    int dimension=2,format=4;
                    texture=create("UnityEngine.Texture2D",texFullCtor,{&dimension,&dimension,&format,&sampling.mipmaps,&sampling.linear});
                    auto data=api.array<uint8_t>("System.Byte",encoded); bool unreadable=true;
                    if(!api.value<bool>(loadImage,nullptr,{texture,data,&unreadable})) throw std::runtime_error("Model texture decode failed");
                }
                api.call(setWrapU,texture,{&sampling.wrapU}); api.call(setWrapV,texture,{&sampling.wrapV});
                api.call(setFilter,texture,{&sampling.filter});
                handle=shareOwned(texture);
            }
            shared->textures[material]=handle;
            textures.push_back(handle); break;
        }
        case ModelBuildPhase::MeshSetup:
            if(gpu) {
                std::vector<Matrix> binds; binds.reserve(asset->bones.size());
                std::vector<void*> bones; bones.reserve(boneRefs.size());
                for(const auto& bone:asset->bones) binds.push_back(bone.inverseBind);
                for(auto bone:boneRefs) bones.push_back(ref(bone));
                buildBindArray=retain(api.array<Matrix>("UnityEngine.Matrix4x4",binds));
                buildBoneArray=retain(api.refs("UnityEngine.Transform",bones));
            }
            break;
        case ModelBuildPhase::Mesh: {
            const auto& p=asset->parts.at(task.index);
            const bool contact=surfaceCollision.affects(task.index),cpuSkin=!gpu || contact;
            auto& cached=shared->meshes[task.index][cpuSkin?1:0];
            uint32_t vertexBuffer=0,normalBuffer=0;
            if(!cached) {
                auto mesh=create("UnityEngine.Mesh",meshCtor);
                std::vector<Vec3> vertices,normals; std::vector<std::array<float,2>> uv;
                vertices.reserve(p.vertexCount); normals.reserve(p.vertexCount); uv.reserve(p.vertexCount);
                struct Weight { std::array<float,4> weights; std::array<int32_t,4> bones; };
                std::vector<Weight> weights; if(!cpuSkin) weights.reserve(p.vertexCount);
                for(size_t i=p.firstVertex;i<p.firstVertex+p.vertexCount;++i) {
                    const auto& v=asset->vertices[i]; vertices.push_back(v.position); normals.push_back(v.normal); uv.push_back(v.uv);
                    if(!cpuSkin) { Weight w{v.weights,{}}; for(size_t j=0;j<4;++j) w.bones[j]=v.bones[j]; weights.push_back(w); }
                }
                auto vertexArray=api.array<Vec3>("UnityEngine.Vector3",vertices);
                auto normalArray=api.array<Vec3>("UnityEngine.Vector3",normals);
                if(cpuSkin) { vertexBuffer=retain(vertexArray); normalBuffer=retain(normalArray); }
                api.call(setVertices,mesh,{vertexArray}); api.call(setNormals,mesh,{normalArray});
                api.call(setUV,mesh,{api.array<std::array<float,2>>("UnityEngine.Vector2",uv)});
                api.call(setTriangles,mesh,{api.array<uint32_t>("System.Int32",std::span(asset->indices).subspan(p.firstIndex,p.indexCount))});
                api.call(bounds,mesh);
                if(!cpuSkin) {
                    api.call(setBind,mesh,{ref(buildBindArray)});
                    api.call(setWeights,mesh,{api.array<Weight>("UnityEngine.BoneWeight",weights)});
                }
                cached=shareOwned(mesh);
                __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SHARED","mesh model=%d part=%zu handle=%u cpuTemplate=%d",selected,task.index,cached,cpuSkin);
            }
            auto mesh=ref(cached);
            if(cpuSkin) {
                // Collision/CPU skin output belongs to this animated instance.
                mesh=api.call(instantiate,nullptr,{mesh}); retain(mesh,true);
                if(markDynamic) api.call(markDynamic,mesh);
                if(!vertexBuffer) {
                    std::vector<Vec3> empty(p.vertexCount);
                    vertexBuffer=retain(api.array<Vec3>("UnityEngine.Vector3",empty));
                    normalBuffer=retain(api.array<Vec3>("UnityEngine.Vector3",empty));
                }
            }
            auto partGO=gameObject("FTREZ.part."+p.name,ref(rootTransform),buildLayer); void* renderer;
            if(!cpuSkin) {
                renderer=api.call(addComponent,partGO,{api.type("UnityEngine.SkinnedMeshRenderer")});
                api.call(setSharedMesh,renderer,{mesh}); api.call(setBones,renderer,{ref(buildBoneArray)});
                api.call(setRootBone,renderer,{ref(rootTransform)}); bool yes=true; api.call(offscreen,renderer,{&yes});
            } else {
                auto filter=api.call(addComponent,partGO,{api.type("UnityEngine.MeshFilter")}); api.call(filterMesh,filter,{mesh});
                renderer=api.call(addComponent,partGO,{api.type("UnityEngine.MeshRenderer")});
            }
            if(!p.renderVisible) { bool visible=false; api.call(setEnabled,renderer,{&visible}); }
            MeshPart part{cpuSkin?retain(mesh):cached,retain(renderer),armTriangles(*asset,p),vertexBuffer,normalBuffer,{},{}};
            part.sharedMesh=!cpuSkin;
            if(!gpu && !contact) { part.cpuVertices.resize(p.vertexCount); part.cpuNormals.resize(p.vertexCount); }
            parts.push_back(std::move(part)); break;
        }
        case ModelBuildPhase::Finish:
            if(buildBoneArray) { release(buildBoneArray); buildBoneArray=0; }
            if(buildBindArray) { release(buildBindArray); buildBindArray=0; }
            break;
        }
    }
    bool advanceBuild() {
        if(buildQueue.done()) return true;
        buildQueue.advanceReady([]{return seconds();},.002,[&](auto task) {
            const auto phase=modelBuildPhaseName(task.phase);
            if(!buildPhase || std::strcmp(buildPhase,phase)) {
                buildPhase=phase;
                __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","stage instance=%zu model=%d phase=%s completed=%zu/%zu",
                    instance,selected,phase,buildQueue.completed(),buildQueue.total());
            }
            DecodedModelTexture decoded;
            if(task.phase==ModelBuildPhase::Texture && textureRequest && task.index==textureRequestSource &&
               !shared->textures[task.index].has_value()) {
                if(!textureRequest->take(decoded)) return false;


                prefetchTexture(task.index+1);
                __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","texture instance=%zu model=%d index=%zu decode=%s width=%d height=%d decode_ms=%.3f",
                    instance,selected,task.index,decoded.pixels?"worker":"unity_fallback",decoded.width,decoded.height,decoded.decodeMs);
            }
            const auto start=seconds();
            buildTask(task,&decoded);
            const double elapsed=(seconds()-start)*1000.; buildCpuMs+=elapsed;
            if(elapsed>=2 || task.phase==ModelBuildPhase::Texture || task.phase==ModelBuildPhase::Mesh)
                __android_log_print(elapsed>=16.7?ANDROID_LOG_WARN:ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD",
                    "unit instance=%zu model=%d phase=%s index=%zu ms=%.3f budget_ms=2",instance,selected,phase,task.index,elapsed);
            return true;
        });
        if(buildQueue.done())
            __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","prepared instance=%zu model=%d cpu_ms=%.3f wall_ms=%.3f",
                instance,selected,buildCpuMs,(seconds()-buildStarted)*1000.);
        return buildQueue.done();
    }
    void validateBuiltHost() {
        auto character=ref(host);
        if(!live(character) || !live(ref(root)) || hostTopology(hostRenderers(character))!=topology)
            throw std::runtime_error("Host changed while model was loading");
    }
    void activateBuilt() {


        for(auto& saved:original) {
            auto renderer=ref(saved.handle); if(!live(renderer)) throw std::runtime_error("Original renderer changed during handoff");
            saved.enabled=api.value<bool>(getEnabled,renderer); saved.changed=false;
        }
        for(auto& saved:animators) {
            auto animator=ref(saved.handle); if(!live(animator)) throw std::runtime_error("Original animator changed during handoff");
            saved.culling=api.value<int>(getCulling,animator); saved.changed=false;
        }
        // New renderers keep Unity's standard probe settings. Do not attach the
        // host's CustomProbeAgent: it supplies a separate game lighting contract.
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL",
            "Lighting instance=%zu: standard Unity/URP; host custom probes disabled",instance);
        findAttachments(ref(host)); nextTopology=seconds()+.5;
    }
    void recordUpdate(double start,uint64_t calls,uint64_t arrays,uint64_t bytes) {
        const double end=seconds(),ms=(end-start)*1000.;
        if(!perf.since) perf.since=start;
        ++perf.frames; perf.totalMs+=ms; perf.maxMs=std::max(perf.maxMs,ms);
        perf.calls+=api.invokeCount-calls; perf.arrays+=api.arrayCount-arrays; perf.bytes+=api.arrayBytes-bytes;
        if(end-perf.since<5) return;
        const double count=double(perf.frames);
        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-PERF",
            "instance=%zu model=%d frames=%llu update_avg_ms=%.3f update_max_ms=%.3f topology_avg_ms=%.3f surface_avg_ms=%.3f bone_write_avg_ms=%.3f invokes_per_frame=%.1f arrays_per_frame=%.2f array_bytes_per_frame=%.0f surface_workers=%d surface_speculative_faces_per_frame=%.1f surface_recomputed_faces_per_frame=%.1f surface_async_frames=%llu surface_wait_avg_ms=%.3f surface_overlap_avg_ms=%.3f gpu_bones=%zu bone_writes_per_frame=%.1f surface_reused_frames=%llu contact_uploads_per_frame=%.1f",
            instance,selected,static_cast<unsigned long long>(perf.frames),perf.totalMs/count,perf.maxMs,
            perf.topologyMs/count,perf.surfaceMs/count,perf.boneWriteMs/count,perf.calls/count,perf.arrays/count,perf.bytes/count,
            int(surfaceCollision.queryWorkers()),perf.speculativeTriangles/count,perf.recomputedTriangles/count,
            static_cast<unsigned long long>(perf.surfaceAsyncFrames),perf.surfaceWaitMs/count,perf.surfaceOverlapMs/count,
            boneUpdates.size(),perf.boneWrites/count,static_cast<unsigned long long>(perf.surfaceReusedFrames),perf.contactUploads/count);
        perf={}; perf.since=end;
    }
    void update(SelfModelOptions options,bool loading=false) {
        struct UpdateTiming {
            Runtime& runtime; double start; uint64_t calls,arrays,bytes;
            ~UpdateTiming() { runtime.recordUpdate(start,calls,arrays,bytes); }
        } timing{*this,seconds(),api.invokeCount,api.arrayCount,api.arrayBytes};
        auto character=ref(host); if(!live(character) || !live(ref(root))) throw std::runtime_error("Character model destroyed");
        const double clock=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(rabbit()) {
            void* hips=nullptr; void* head=nullptr;
            for(size_t i=0;i<asset->bones.size();++i) {
                if(asset->bones[i].human==0) hips=ref(hostBones[i]);
                if(asset->bones[i].human==10) head=ref(hostBones[i]);
            }
            if(!live(hips) || !live(head)) throw std::runtime_error("Rabbit skeleton changed");
            const auto size=rabbitSize(glm::length(worldPosition(head)-worldPosition(hips)),rabbitBaseline);
            if(size==RabbitSize::Rebuild) throw std::runtime_error("Rabbit grew; rebuilding model");
            if(size==RabbitSize::Hidden) {
                bool no=false; api.call(setActive,ref(root),{&no});
                dynamics.reset(); lastPoseTime=0; report("Rabbit shrunk; replacement hidden"); return;
            }
        }
        if(clock>=nextTopology) {
            const double start=seconds();
            nextTopology=clock+.5;
            if(hostTopology(hostRenderers(character))!=topology) {
                character=api.hold(character);
                if(!parkSnapshot()) throw std::runtime_error("Host renderers loaded or changed");
                restoreSnapshot(character);
            }
            findAttachments(character);
            perf.topologyMs+=(seconds()-start)*1000.;
        }
        const int effectiveStyle=modelStyle(*asset,options.style);
        bool shaderLost=false;
        if(effectiveStyle==4 && clock>=nextStyleProbe) {
            shaderLost=!usableShader(ref(customShaderHandle));
            if(!shaderLost && styleNotice.empty()) nextStyleProbe=clock+5;
        }
        const auto sharedStyle=shared->styles.find({effectiveStyle,options.hairTone});
        const bool sharedStyleChanged=sharedStyle!=shared->styles.end() &&
            (sharedStyleGeneration!=sharedStyle->second.generation ||
             ((lobbyHost || shared->hostMaterialLobby) && !sharedStyle->second.lobby));
        if(appliedStyle!=effectiveStyle || appliedHairTone!=options.hairTone || shaderLost || sharedStyleChanged || (!styleNotice.empty() && clock>=nextStyleProbe)) {
            restyle(effectiveStyle,options.hairTone); nextStyleProbe=clock+5;
        }
        auto inverse=matrix(transformMatrix(character,true));
        std::vector<std::optional<Matrix>> now(hostBones.size());
        for(size_t i=0;i<hostBones.size();++i) if(hostBones[i]) {
            auto b=ref(hostBones[i]); if(!live(b)) throw std::runtime_error("Character skeleton changed");
            now[i]=stored(inverse*matrix(transformMatrix(b)));
        }
        if(const auto error=hostPoseError(*asset,rest,now)) {
            dynamics.reset(); lastPoseTime=0;
            if(!rejectedPoses) __android_log_print(ANDROID_LOG_WARN,"FTREZ-MODEL",
                "host_pose_rejected instance=%zu model=%d reason=%s",instance,selected,error);
            if(++rejectedPoses>=3) throw std::runtime_error(error);
            report(std::string(error)+"; holding last valid pose");
            return;
        }
        rejectedPoses=0;
        auto pose=retarget(*asset,rest,now);

        Vec3 offset{0,options.offset,0};
        const float scale=fit*options.scale;
        const auto livePositions=humanPositions(*asset,&now);
        const auto pelvis=fittedPelvis(livePositions,modelPositions,modelLengths,hostLengths,scale,rabbit());
        for(size_t i=0;i<asset->bones.size();++i) if(boneKey(asset->bones[i])=="pelvis" && now[i]) {
            offset=vec(pelvis-vec(pose[i].position)*scale+glm::vec3(0,options.offset,0)); break;
        }
        const auto hostWorld=glm::inverse(inverse);
        followHost(hostWorld);
        Vec3 size{scale,scale,scale}; api.call(setScale,ref(rootTransform),{&size});
        transforms.write(api,ref(rootTransform),setPosition,setRotation,offset,Quat{});
        const auto modelWorld=hostWorld*glm::translate(glm::mat4(1),vec(offset))*
            glm::scale(glm::mat4(1),glm::vec3(scale));
        armAvoidance.update(*asset,pose);
        dynamics.update(*asset,pose,now,modelWorld,glm::vec3(hostWorld[3]),lastPoseTime>0?float(clock-lastPoseTime):0.f);
        lastPoseTime=clock;
        updateAttachments(options.hideOriginal,pose,now,modelWorld,hostWorld);
        if(!playerView && !instance && mainCamera && headIndex>=0 && neckIndex>=0 && headRadius>0 && now[headIndex]) {
            auto camera=api.call(mainCamera);
            if(live(camera)) {
                auto tf=api.call(getTransform,api.call(componentGO,camera));
                const auto cameraPosition=glm::vec3(matrix(transformMatrix(tf))[3]);
                const auto modelHead=glm::vec3(modelWorld*glm::vec4(vec(pose[headIndex].position),1));
                const auto modelNeck=glm::vec3(modelWorld*glm::vec4(vec(pose[neckIndex].position),1));
                const float worldScale=glm::length(glm::vec3(modelWorld[0]));
                const float radius=headRadius*worldScale;
                const auto center=modelHead+direction(modelHead-modelNeck)*radius;
                const auto hostHead=glm::vec3(hostWorld*matrix(*now[headIndex])[3]);
                firstPerson=nearHead(firstPerson,glm::length(cameraPosition-hostHead),glm::length(cameraPosition-center),radius,bodyHeight*worldScale);
            } else firstPerson=false;
        }
        if(playerView) firstPerson=playerView->firstPerson;
        if(firstPerson!=appliedFirstPerson) {
            for(size_t i=0;i<parts.size();++i) {
                const auto& p=asset->parts[i];
                auto& part=parts[i];
                // First-person index edits must never trim another slot.
                makeModelMeshWritable(part.mesh,part.sharedMesh,[&](uint32_t source) {
                    return retain(api.call(instantiate,nullptr,{ref(source)}),true);
                },[&](uint32_t local) {
                    api.call(setSharedMesh,ref(part.renderer),{ref(local)});
                });
                const auto triangles=firstPerson?std::span<const uint32_t>(parts[i].arms):
                    std::span<const uint32_t>(asset->indices).subspan(p.firstIndex,p.indexCount);
                api.call(setTriangles,ref(parts[i].mesh),{api.array<uint32_t>("System.Int32",triangles)});
            }
            appliedFirstPerson=firstPerson;
        }
        const auto writeBones=[&] {
            if(!gpu) return;
            const double start=seconds();
            perf.boneWrites+=boneUpdates.apply(pose,[&](size_t i,const PoseBone& bone) {
                transforms.write(api,ref(boneRefs[i]),setPosition,setRotation,bone.position,bone.rotation);
            });
            perf.boneWriteMs+=(seconds()-start)*1000.;
        };
        if(!gpu || surfaceCollision.active()) {
            bool visibleContact=false;
            for(size_t i=0;i<parts.size();++i)
                visibleContact|=surfaceCollision.affects(i) && (!firstPerson || !parts[i].arms.empty());
            auto matrices=skinMatrices(*asset,pose);




            if(visibleContact && surfaceWorker) {
                double surfaceStart=0,surfaceEnd=0,boneStart=0,boneEnd=0;
                surfaceWorker->run([&] {
                    surfaceStart=seconds(); surfaceCollision.update(*asset,matrices); surfaceEnd=seconds();
                },[&] {
                    boneStart=seconds(); writeBones(); boneEnd=seconds();
                });
                perf.surfaceMs+=(surfaceEnd-surfaceStart)*1000.;
                perf.surfaceWaitMs+=(seconds()-boneEnd)*1000.;
                perf.surfaceOverlapMs+=std::max(0.,std::min(surfaceEnd,boneEnd)-std::max(surfaceStart,boneStart))*1000.;
                ++perf.surfaceAsyncFrames;
            } else {
                writeBones();
                if(visibleContact) {
                    const double start=seconds(); surfaceCollision.update(*asset,matrices);
                    const double elapsed=(seconds()-start)*1000.;
                    perf.surfaceMs+=elapsed; perf.surfaceWaitMs+=elapsed;
                }
            }
            if(visibleContact) {
                perf.speculativeTriangles+=surfaceCollision.speculativeTriangles();
                perf.recomputedTriangles+=surfaceCollision.recomputedTriangles();
                perf.surfaceReusedFrames+=surfaceCollision.reusedResult();
            }
            for(size_t i=0;i<asset->parts.size();++i) {
                const bool contact=surfaceCollision.affects(i);
                if(gpu && !contact) continue;
                if(firstPerson && parts[i].arms.empty()) continue;
                const auto& p=asset->parts[i]; auto& part=parts[i];
                std::span<const Vec3> vertices,normals;
                if(contact) {
                    if(part.contactRevision==surfaceCollision.revision()) continue;
                    vertices=std::span<const Vec3>(surfaceCollision.positions()).subspan(p.firstVertex,p.vertexCount);
                    normals=std::span<const Vec3>(surfaceCollision.normals()).subspan(p.firstVertex,p.vertexCount);
                } else {
                    for(size_t j=0;j<p.vertexCount;++j) {
                        auto [v,n]=skin(asset->vertices[p.firstVertex+j],matrices);
                        part.cpuVertices[j]=v; part.cpuNormals[j]=n;
                    }
                    vertices=part.cpuVertices; normals=part.cpuNormals;
                }
                auto mesh=ref(part.mesh);
                api.call(setVertices,mesh,{api.writeArray<Vec3>(ref(part.vertices),vertices)});
                api.call(setNormals,mesh,{api.writeArray<Vec3>(ref(part.normals),normals)}); api.call(bounds,mesh);
                if(contact) { part.contactRevision=surfaceCollision.revision(); ++perf.contactUploads; }
            }
        } else writeBones();


        for(auto& s:animators) {
            auto animator=ref(s.handle);
            if(!live(animator)) throw std::runtime_error("Original animator changed");
            int current=api.value<int>(getCulling,animator);
            if(!s.changed || current!=0) s.culling=current;
            if(options.hideOriginal && current!=0) {
                s.changed=true; int alwaysAnimate=0; api.call(setCulling,animator,{&alwaysAnimate});
            } else if(!options.hideOriginal && s.changed) {
                if(current==0) api.call(setCulling,animator,{&s.culling});
                s.changed=false;
            }
        }
        bool visible=api.value<bool>(activeHierarchy,api.call(componentGO,character));
        bool any=false;
        for(auto& s:original) {
            auto renderer=ref(s.handle); if(!live(renderer)) throw std::runtime_error("Original renderers changed");
            bool current=api.value<bool>(getEnabled,renderer);
            applyVisibility(s,current,options.hideOriginal,[&](bool enabled) { api.call(setEnabled,renderer,{&enabled}); });
            any|=s.enabled && api.value<bool>(activeHierarchy,api.call(componentGO,renderer));
        }
        visible=modelVisible(visible,any,playerView); api.call(setActive,ref(root),{&visible});
        if(!loading) report(modelLabel(selected)+"; "+styleName(appliedStyle)+"; "+(gpu?"GPU":"CPU")+"; bones "+std::to_string(matched)+styleNotice+(surfaceCollision.active()?"; hook surface collision":"")+(firstPerson?"; first-person arms":""));
    }
};
double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
void setResourcePath(std::string path) {
    if(path.empty()) throw std::invalid_argument("Model resource path is empty");
    resourcePath=std::move(path);
}
SelfModelControls uiControls() {
    return {settings,configure,[]{
        auto& s=state(); std::lock_guard lock(s.mutex);
        std::string message;
        if(s.switches.requested().enabled || s.switches.requested().rabbitEnabled) message=lobbyRenderStatus();
        if(s.switches.requested().enabled) message+=(message.empty()?"":" | ")+s.slots[0].message+" | Profile: "+s.slots[2].message;
        if(s.switches.requested().rabbitEnabled) message+=(message.empty()?"":" | ")+std::string("Rabbit: ")+s.slots[1].message;
        if(!s.scanMessage.empty()) message+=(message.empty()?"":" | ")+s.scanMessage;
        return message.empty()?s.slots[0].message:message;
    },choices};
}
bool wanted() { const auto v=settings(); return v.enabled || v.rabbitEnabled; }
void tick(const std::vector<void*>& images,uintptr_t character,bool matchActive,int frame,uintptr_t actor) {
    static auto* instances=new std::array<std::unique_ptr<Runtime>,3>{
        std::make_unique<Runtime>(0),std::make_unique<Runtime>(1),std::make_unique<Runtime>(2)};
    SelfModelOptions allOptions; uint64_t refresh;
    { auto& s=state(); std::lock_guard lock(s.mutex); allOptions=s.switches.applied(); refresh=s.refresh; }
    for(auto& slot:*instances) {

        std::unique_ptr<Runtime> retired;
        auto* r=slot.get();
        const bool requested=refresh!=r->lastRefresh;
        if(frame>=0 && r->lastFrame==frame && !requested) continue;
        r->lastFrame=frame; r->lastRefresh=refresh;
        if(requested) r->retry=r->pendingRetry=0;
        const auto options=modelInstanceOptions(allOptions,r->rabbit());
        if(!options.enabled && !r->pending && !r->host && !r->rabbitTarget && !r->previewUI && r->owned.empty() && r->retained.empty() && !r->restorePending) {
            r->report("Disabled; original model"); continue;
        }
        ModelTemps temps{r->api};
        try {
            if(!options.enabled) {
                if(r->rabbit()) r->dropRabbit();
                r->dropPreview();
                const bool pendingClean=r->discardPending(),clean=r->cleanup();
                r->report(clean&&pendingClean?"Disabled; original model restored":"Restoring original model; retry pending"); continue;
            }
            if(!modelAvailable(options.model)) {
                const bool pendingClean=r->discardPending(),clean=r->cleanup();
                if(!clean || !pendingClean) throw std::runtime_error("Original model restore pending");
                r->report("Selected model unavailable; check model.json"); continue;
            }
            if(seconds()<r->retry) continue;
            if(r->restorePending && !r->cleanup()) throw std::runtime_error("Original model restore pending");
            if(!r->bound) r->bind(images);
            std::shared_ptr<const Model> latest;
            if(r->selected!=options.model || requested || r->pending || r->snapshotParked)
                latest=resource(r->instance,options.model);
            if(r->snapshotParked && (r->selected!=options.model || latest!=r->asset ||
               !r->live(r->ref(r->root)) || !r->live(r->ref(r->carrier)))) {
                __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
                    "invalidated instance=%zu model=%d requested=%d",r->instance,r->selected,options.model);
                if(!r->cleanup()) throw std::runtime_error("Snapshot release pending");
            }
            void* target=r->selectTarget(character,matchActive,frame);
            if(r->instance!=0) r->lobbyHost=false;
            else if(requested || target!=r->lobbyCheckedHost || seconds()>=r->nextLobbyCheck) {
                r->lobbyHost=isLobbyModelHost(images,modelSymbols,target);
                r->lobbyCheckedHost=target;
                r->nextLobbyCheck=seconds()+.5;
            }
            r->playerView=r->instance==0 && target==reinterpret_cast<void*>(character)?
                r->game.view(r->api,reinterpret_cast<void*>(actor)):std::nullopt;



            if(!r->rabbit() && r->active(target) &&
               (!r->root || r->selected!=options.model)) {
                const size_t destination=r->instance,source=destination==0?2:0;
                auto& donorSlot=(*instances)[source]; auto* donor=donorSlot.get();
                bool available=false;
                if(donor->bound && donor->selected==options.model && donor->asset &&
                   !donor->pending && !donor->restorePending && donor->buildQueue.done() &&
                   resource(destination,options.model)==donor->asset) {
                    ModelTemps donorTemps{donor->api};
                    auto donorTarget=donor->selectTarget(character,matchActive,frame);

                    if(donor->snapshotParked || !donor->active(donorTarget) || donor->ref(donor->host)!=donorTarget)
                        available=donor->live(donor->ref(donor->root)) && donor->parkSnapshot();
                }
                if(available) {
                    const auto newest=modelInstanceOptions(settings(),false);
                    if(!newest.enabled || newest.model!=options.model || resource(destination,options.model)!=donor->asset) continue;
                    auto empty=std::make_unique<Runtime>(source);
                    if(!r->discardPending() || !r->cleanup()) throw std::runtime_error("Snapshot handoff restore pending");
                    donor->dropPreview(); donor->previewUI=std::exchange(r->previewUI,0);
                    donor->playerView=r->playerView; donor->lobbyHost=r->lobbyHost; donor->instance=destination;
                    donor->lastFrame=frame; donor->lastRefresh=refresh; donor->retry=0;
                    retired=std::move(slot); slot=std::move(donorSlot); donorSlot=std::move(empty);
                    r=slot.get(); ModelTemps snapshotTemps{r->api};
                    const double start=seconds();
                    try {
                        r->restoreSnapshot(target); r->update(options);
                        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
                            "transferred from=%zu to=%zu model=%d first_update_ms=%.3f texture_uploads=0 mesh_builds=0",
                            source,destination,r->selected,(seconds()-start)*1000.);
                    } catch(const std::exception& e) {
                        if(!r->restoreHost()) throw;
                        r->releaseHost(); r->snapshotParked=true; r->retry=seconds()+.25;
                        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
                            "rebind_wait instance=%zu model=%d reason=%s",destination,r->selected,e.what());
                        r->report("Waiting for character skeleton");
                    }
                    continue;
                }
            }
            if(!r->live(target) || (!r->rabbit() && !r->active(target))) {
                const bool pendingClean=r->discardPending();
                const bool reusable=r->selected==options.model && (!requested || latest==r->asset);
                const bool clean=(reusable && r->parkSnapshot()) || r->cleanup();
                if(!clean || !pendingClean) throw std::runtime_error("Original model restore pending");
                r->report(r->rabbit()?(matchActive?"No Lily rabbit in the scene":"Waiting for a match"):
                    r->instance==2?"Waiting for player profile":"Waiting for own character or lobby model");
                r->retry=0; continue;
            }
            if(r->host && r->ref(r->host)!=target) {
                const bool pendingClean=r->discardPending();
                const bool reusable=r->selected==options.model && (!requested || latest==r->asset);
                const bool clean=(reusable && r->parkSnapshot()) || r->cleanup();
                if(!clean || !pendingClean) throw std::runtime_error("Original model restore pending");
            }
            if(r->snapshotParked) {
                const auto newest=modelInstanceOptions(settings(),r->rabbit());
                if(!newest.enabled || newest.model!=options.model || resource(r->instance,options.model)!=r->asset) continue;
                try { r->restoreSnapshot(target); }
                catch(const std::exception& e) {


                    if(!r->restoreHost()) throw;
                    r->releaseHost(); r->snapshotParked=true;
                    __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-SNAPSHOT",
                        "rebind_wait instance=%zu model=%d reason=%s",r->instance,r->selected,e.what());
                    r->report("Waiting for character skeleton"); r->retry=seconds()+.25; continue;
                }
            }
            const bool replace=r->selected!=options.model || (latest && latest!=r->asset);
            if(!replace) {
                if(!r->discardPending()) throw std::runtime_error("Cancelled model cleanup pending");
                r->update(options); continue;
            }
            bool loadFailed=false;
            try {
                if(r->pending && (r->pending->restorePending || r->pending->selected!=options.model ||
                    r->pending->ref(r->pending->host)!=target || (latest && latest!=r->pending->asset))) {
                    __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","cancel instance=%zu model=%d",r->instance,r->pending->selected);
                    if(!r->discardPending()) throw std::runtime_error("Pending model cleanup retry");
                }
                if(latest && !r->pending && seconds()>=r->pendingRetry) {
                    r->pending=std::make_unique<Runtime>(r->instance);
                    ModelTemps pendingTemps{r->pending->api};
                    const double start=seconds(); r->pending->bind(images);
                    __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","bind instance=%zu model=%d ms=%.3f",
                        r->instance,options.model,(seconds()-start)*1000.);
                    r->pending->lobbyHost=r->lobbyHost;
                    r->pending->beginBuild(target,latest,options.model);
                }
                bool ready=false;
                if(r->pending) {
                    ModelTemps pendingTemps{r->pending->api};
                    ready=r->pending->advanceBuild();
                }
                if(ready) {
                    const auto newest=modelInstanceOptions(settings(),r->rabbit());


                    if(newest.enabled && newest.model==options.model && resource(r->instance,options.model)==r->pending->asset) {
                        auto& candidate=*r->pending;
                        ModelTemps pendingTemps{candidate.api};
                        candidate.validateBuiltHost();
                        if(!r->cleanup()) throw std::runtime_error("Original model restore pending at handoff");
                        const double start=seconds();
                        candidate.playerView=r->playerView; candidate.lobbyHost=r->lobbyHost;
                        candidate.activateBuilt();


                        candidate.update(options);
                        __android_log_print(ANDROID_LOG_INFO,"FTREZ-MODEL-LOAD","activated instance=%zu model=%d ms=%.3f",
                            r->instance,options.model,(seconds()-start)*1000.);
                        candidate.lastFrame=frame; candidate.lastRefresh=refresh;
                        candidate.rabbitTarget=std::exchange(r->rabbitTarget,0);
                        candidate.nextRabbitSearch=r->nextRabbitSearch;
                        candidate.previewUI=std::exchange(r->previewUI,0);
                        retired=std::move(slot); slot=std::move(retired->pending);
                        continue;
                    }
                }
            } catch(const std::exception& e) {
                loadFailed=true;
                __android_log_print(ANDROID_LOG_ERROR,"FTREZ-MODEL-LOAD","failed instance=%zu model=%d phase=%s reason=%s",
                    r->instance,options.model,r->pending&&r->pending->buildPhase?r->pending->buildPhase:"bind",e.what());
                const bool clean=r->discardPending(); r->pendingRetry=seconds()+1.5;
                r->report(std::string(e.what())+(clean?"; load retry pending":"; pending model cleanup retry"));
            }
            if(r->root && r->asset && !r->restorePending) r->update(options,true);
            if(!loadFailed && r->pending) {
                const auto& queue=r->pending->buildQueue;
                r->report("Loading "+modelLabel(options.model)+"; "+std::to_string(queue.total()?queue.completed()*100/queue.total():0)+"%");
            }
        } catch(const std::exception& e) {
            ModelTemps failureTemps{r->api};
            const bool pendingClean=r->discardPending(),restored=r->cleanup();
            __android_log_print(ANDROID_LOG_WARN,"FTREZ-MODEL-LOAD","runtime reset instance=%zu model=%d reason=%s pending_clean=%d",
                r->instance,options.model,e.what(),pendingClean);
            r->report(std::string(e.what())+(restored?"; original retained":"; restore retry pending")); r->retry=seconds()+1.5;
        }
    }
    // Read every final slot after all updates: Profile can transfer slot 0's
    // snapshot, and Rabbit is allowed to run while Self is disabled.
    ModelTemps lobbyTemps0{(*instances)[0]->api},lobbyTemps1{(*instances)[1]->api},lobbyTemps2{(*instances)[2]->api};
    std::array<LobbyRenderTarget,3> lobbyTargets;
    if(allOptions.lobbyRendering) {
        for(size_t slot=0;slot<instances->size();++slot) {
            auto& model=*(*instances)[slot]; auto& target=lobbyTargets[slot];
            try {
                const auto options=modelInstanceOptions(allOptions,model.rabbit());
                if(!options.enabled || !model.bound || !model.host || !model.root || !model.asset ||
                   model.snapshotParked || model.restorePending || !model.active(model.ref(model.host))) continue;
                target.host=model.ref(model.host); target.renderers.reserve(model.parts.size());
                for(const auto& part:model.parts) if(auto renderer=model.ref(part.renderer))
                    if(model.live(renderer)) target.renderers.push_back(renderer);
            } catch(const std::exception& e) {
                target={};
                __android_log_print(ANDROID_LOG_WARN,"FTREZ-LOBBY","Target %zu unavailable: %s",slot,e.what());
            }
        }
    }
    updateLobbyRender(images,modelSymbols,!matchActive,lobbyTargets,allOptions.lobbyRendering);
}
}
