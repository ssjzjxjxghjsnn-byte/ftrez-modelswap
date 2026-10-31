#include "il2cpp/frame_hook.h"
#include "il2cpp/il2cpp_api.h"
#include "self_model/unity_api.h"
#include "dobby.h"
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ft::il2cpp::frame_hook {
namespace {
using RenderLoop = void (*)(void*, intptr_t, void*, const void*);
using PreCull = void (*)(void*, const void*);
struct State {
    std::mutex installMutex;
    std::atomic<bool> ready{false}, callbackErrorLogged{false};
    bool attempted = false;
    RenderLoop renderLoop = nullptr;
    PreCull preCull = nullptr;
    std::function<void()> callback;
};
State& state() { static auto* value = new State; return *value; }
thread_local bool inCallback = false;
struct CallbackScope {
    CallbackScope() { inCallback = true; }
    ~CallbackScope() { inCallback = false; }
};

void beforeRender() noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire) || inCallback) return;
    CallbackScope scope;
    try { s.callback(); }
    catch (...) {
        if (!s.callbackErrorLogged.exchange(true))
            __android_log_print(ANDROID_LOG_INFO, "FTREZ-ModelSwap", "Before-render callback failed; Unity rendering continues");
    }
}
void renderLoop(void* pipeline, intptr_t loopPointer, void* request, const void* method) {
    RenderLoop original;
    { auto& s = state(); std::lock_guard lock(s.installMutex); original = s.renderLoop; }
    beforeRender();
    original(pipeline, loopPointer, request, method);
}
void preCull(void* camera, const void* method) {
    PreCull original;
    { auto& s = state(); std::lock_guard lock(s.installMutex); original = s.preCull; }
    beforeRender();
    original(camera, method);
}

struct Entries { void* renderLoop; void* preCull; };
Entries resolve(const std::vector<void*>& images) {
    auto imageName = reinterpret_cast<const char* (*)(void*)>(lookup("il2cpp_image_get_name"));
    auto methodClass = reinterpret_cast<void* (*)(void*)>(lookup("il2cpp_method_get_class"));
    auto thread = reinterpret_cast<void* (*)()>(lookup("il2cpp_thread_current"));
    if (!imageName || !methodClass || !thread) throw std::runtime_error("Frame-hook metadata API unavailable");
    if (!thread()) throw std::runtime_error("Frame-hook installation requires an attached IL2CPP thread");
    void* core = nullptr;
    for (auto image : images) if (image) {
        const auto name = imageName(image);
        if (name && !std::strcmp(name, "UnityEngine.CoreModule.dll")) {
            if (core && core != image) throw std::runtime_error("Ambiguous Unity core assembly");
            core = image;
        }
    }
    if (!core) throw std::runtime_error("UnityEngine.CoreModule.dll is unavailable");
    self_model::UnityApi api;
    api.bind({core});
    auto srp = api.method("UnityEngine.Rendering.RenderPipelineManager", "DoRenderLoop_Internal",
        {"UnityEngine.Rendering.RenderPipelineAsset", "System.IntPtr", "UnityEngine.Object"}, "System.Void", true);
    auto camera = api.method("UnityEngine.Camera", "FireOnPreCull", {"UnityEngine.Camera"}, "System.Void", true);
    if (methodClass(srp) != api.klass("UnityEngine.Rendering.RenderPipelineManager") ||
            methodClass(camera) != api.klass("UnityEngine.Camera"))
        throw std::runtime_error("Frame-hook declaring class mismatch");
    Entries entries{methodPointer(srp), methodPointer(camera)};
    if (!entries.renderLoop || !entries.preCull || entries.renderLoop == entries.preCull ||
            !executable(reinterpret_cast<uintptr_t>(entries.renderLoop), 16) ||
            !executable(reinterpret_cast<uintptr_t>(entries.preCull), 16))
        throw std::runtime_error("Frame-hook native entry points are unavailable or shared");
    return entries;
}
}

void install(const std::vector<void*>& images, std::function<void()> callback) {
    if (!callback) throw std::invalid_argument("Before-render callback is required");
    auto& s = state();
    std::lock_guard lock(s.installMutex);
    if (s.ready.load(std::memory_order_acquire)) return;
    if (s.attempted) throw std::runtime_error("A previous frame-hook installation failed");
    const auto entries = resolve(images);
    s.callback = std::move(callback);
    s.attempted = true;
    dobby_dummy_func_t original = nullptr;
    // Bundled Dobby rejects invalid relocation before committing any detour.
    // A commit error can occur after the entry was written, so retain its valid
    // original even on failure. Detours wait on installMutex for publication.
    int result = DobbyHook(entries.renderLoop, reinterpret_cast<dobby_dummy_func_t>(renderLoop), &original);
    s.renderLoop = reinterpret_cast<RenderLoop>(original);
    if (result || !s.renderLoop) {
        s.callback = {};
        throw std::runtime_error("Cannot install Unity SRP frame hook");
    }
    original = nullptr;
    result = DobbyHook(entries.preCull, reinterpret_cast<dobby_dummy_func_t>(preCull), &original);
    s.preCull = reinterpret_cast<PreCull>(original);
    if (result || !s.preCull) {
        s.callback = {};
        throw std::runtime_error("Cannot install Unity camera pre-cull hook");
    }
    // Both trampolines and the immutable callback are fully published together.
    s.ready.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, "FTREZ-ModelSwap", "Unity before-render hooks ready");
}
}
