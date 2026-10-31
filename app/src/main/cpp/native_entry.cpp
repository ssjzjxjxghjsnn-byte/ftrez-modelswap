#include "il2cpp/il2cpp_api.h"
#include "il2cpp/method_invoke.h"
#include "sdk/model_bindings.h"
#include "sdk/own_player.h"
#include "self_model/model_runtime.h"
#include "self_model/unity_api.h"
#include "settings/native_settings.h"
#include <android/log.h>
#include <jni.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace ft {
namespace {
using Clock=std::chrono::steady_clock;
void log(int priority,const std::string& message) {
    __android_log_print(priority,"FTREZ-ModelSwap","%s",message.c_str());
}
template<class T> T api(const char* name) {
    auto pointer=reinterpret_cast<T>(il2cpp::lookup(name));
    if(!pointer) throw std::runtime_error(std::string("IL2CPP API unavailable: ")+name);
    return pointer;
}
struct AttachedThread {
    void* thread=nullptr;
    void (*detach)(void*)=nullptr;
    ~AttachedThread() { if(thread && detach) detach(thread); }
};
struct Runtime {
    self_model::UnityApi unity;
    sdk::OwnPlayer player;
    std::vector<void*> images;
    void *mainThreadCheck=nullptr,*frameCount=nullptr;
    std::atomic<bool> ready{false};
    std::atomic<pid_t> mainThread{0};
    int lastFrame=-1;
    bool firstTickLogged=false;
    Clock::time_point nextError{};

    bool onMainThread() {
        if(!ready.load(std::memory_order_acquire)) return false;
        const auto tid=gettid();
        if(mainThread.load(std::memory_order_relaxed)==tid) return true;
        const auto result=il2cpp::Invoke(mainThreadCheck);
        if(!result.succeeded() || !result.value) return false;
        const auto unboxed=unity.object_unbox(result.value);
        bool isMain=false;
        if(unboxed) std::memcpy(&isMain,unboxed,sizeof(isMain));
        if(isMain) mainThread.store(tid,std::memory_order_relaxed);
        return isMain;
    }
    void error(const std::exception& e) {
        const auto now=Clock::now();
        if(now>=nextError) { nextError=now+std::chrono::seconds(5);log(ANDROID_LOG_ERROR,e.what()); }
    }
    void tick() {
        // The only owner of Unity object work is the verified Unity thread.
        try {
            const auto frame=unity.value<int>(frameCount);
            unity.clearTemps();
            if(frame==lastFrame) return;
            lastFrame=frame;
            if(!firstTickLogged) {
                firstTickLogged=true;
                log(ANDROID_LOG_INFO,"Unity main-thread frame callback active");
            }
            settings::tick();
            sdk::OwnPlayerState own;
            try { own=player.update(); }
            catch(const std::exception& e) {
                error(e);
                // Suppress choosing a lobby host while gameplay identity is
                // uncertain, and allow the model engine to restore its host.
                own.matchActive=true;
            }
            self_model::tick(images,own.character,own.matchActive,frame,own.actor);
        } catch(const std::exception& e) { error(e); }
        unity.clearTemps();
    }
};
Runtime& runtime() { static auto* value=new Runtime;return *value; }

std::vector<void*> collectImages(void* domain) {
    const auto assemblies=api<const void* const*(*)(void*,size_t*)>("il2cpp_domain_get_assemblies");
    const auto image=api<void*(*)(const void*)>("il2cpp_assembly_get_image");
    size_t count=0;
    const auto list=assemblies(domain,&count);
    if(!list || count==0) return {};
    if(count>4096) throw std::runtime_error("Invalid loaded assembly count");
    std::vector<void*> result;result.reserve(count);
    for(size_t i=0;i<count;++i) if(list[i]) if(auto value=image(list[i])) result.push_back(value);
    return result;
}
bool assembliesReady(const std::vector<void*>& images,const std::set<std::string>& required) {
    const auto name=api<const char*(*)(void*)>("il2cpp_image_get_name");
    std::set<std::string> loaded;
    for(auto image:images) if(auto value=name(image)) loaded.emplace(value);
    for(const auto& value:required) if(!loaded.count(value)) return false;
    return true;
}
void bootstrap(const std::string& filesDir,const std::string& modelDir) {
    try {
        // A bad or missing external config fails before installing any hook.
        sdk::loadConfig(modelDir+"/config.json");
        self_model::setResourcePath(modelDir);
        settings::initialize(filesDir);
        std::set<std::string> required{sdk::assemblyName(),"UnityEngine.CoreModule.dll","UnityEngine.UI.dll"};
        for(const auto* bindings:{&sdk::modelBindings(),&sdk::ownPlayerBindings(),&sdk::settingsBindings()})
            for(const auto& [key,symbol]:bindings->types) required.insert(symbol.assembly);
        const auto deadline=Clock::now()+std::chrono::minutes(5);
        while(!il2cpp::resolve().resolved) {
            if(Clock::now()>=deadline) throw std::runtime_error("Timed out waiting for IL2CPP exports");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        const auto domainGet=api<void*(*)()>("il2cpp_domain_get");
        const auto threadAttach=api<void*(*)(void*)>("il2cpp_thread_attach");
        AttachedThread attached;
        attached.detach=api<void(*)(void*)>("il2cpp_thread_detach");
        auto& r=runtime();
        log(ANDROID_LOG_INFO,"Waiting for the configured game assemblies");
        for(;;) {
            auto domain=domainGet();
            if(domain) {
                auto images=collectImages(domain);
                if(!images.empty() && !attached.thread) {
                    attached.thread=threadAttach(domain);
                    if(!attached.thread) throw std::runtime_error("Cannot attach the IL2CPP bootstrap thread");
                }
                if(attached.thread && assembliesReady(images,required)) { r.images=std::move(images);break; }
            }
            if(Clock::now()>=deadline) throw std::runtime_error("Timed out waiting for configured assemblies; check config.json");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        sdk::validateBindings(r.images,sdk::modelBindings());
        sdk::validateBindings(r.images,sdk::ownPlayerBindings());
        sdk::validateBindings(r.images,sdk::settingsBindings());
        log(ANDROID_LOG_INFO,"Configured game bindings validated");
        r.unity.bind(r.images);
        r.mainThreadCheck=r.unity.method("UnityEngine.Object","CurrentThreadIsMainThread",{},"System.Boolean",true);
        r.frameCount=r.unity.method("UnityEngine.Time","get_frameCount",{},"System.Int32",true);
        r.player.bind(r.images);
        self_model::prepareBindings(sdk::modelBindings());
        settings::install(r.images);
        self_model::installMethodHooks(r.images,[&r] { return r.onMainThread(); },[&r] { r.tick(); });
        r.ready.store(true,std::memory_order_release);
        log(ANDROID_LOG_INFO,"Model swap ready: native graphics settings, gameplay, lobby and own profile; config="+modelDir+"/config.json");
    } catch(const std::exception& e) {
        log(ANDROID_LOG_ERROR,std::string("Model swap startup stopped: ")+e.what());
    } catch(...) {
        log(ANDROID_LOG_ERROR,"Model swap startup stopped: unknown native exception");
    }
}
std::string stringArgument(JNIEnv* env,jstring value) {
    if(!value) throw std::invalid_argument("Native model path is null");
    const auto chars=env->GetStringUTFChars(value,nullptr);
    if(!chars) throw std::runtime_error("Cannot read native model path");
    std::string path(chars);env->ReleaseStringUTFChars(value,chars);
    if(path.empty() || path.front()!='/') throw std::invalid_argument("Native model path must be absolute");
    return path;
}
}
}

extern "C" JNIEXPORT void JNICALL
Java_com_ftrez_modelswap_NativeBridge_start(JNIEnv* env,jclass,jstring filesDir,jstring modelDir) {
    static std::atomic<bool> started{false};
    try {
        auto files=ft::stringArgument(env,filesDir),models=ft::stringArgument(env,modelDir);
        if(started.exchange(true)) return;
        try { std::thread([files=std::move(files),models=std::move(models)] { ft::bootstrap(files,models); }).detach(); }
        catch(...) { started.store(false);throw; }
    } catch(const std::exception& e) {
        if(!env->ExceptionCheck()) if(auto type=env->FindClass("java/lang/IllegalStateException")) env->ThrowNew(type,e.what());
    }
}
