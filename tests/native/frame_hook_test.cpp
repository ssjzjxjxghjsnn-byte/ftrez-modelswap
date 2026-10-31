// Exercises the production frame_hook.cpp and its real UnityApi resolver.
// Only external IL2CPP exports, executable mapping and Dobby are faked.
#include "il2cpp/frame_hook.h"
#include "il2cpp/il2cpp_api.h"
#include "dobby.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Srp = void (*)(void*, intptr_t, void*, const void*);
using Camera = void (*)(void*, const void*);
struct Type { std::string name; } voidType{"System.Void"}, intType{"System.Int32"},
    assetType{"UnityEngine.Rendering.RenderPipelineAsset"}, pointerType{"System.IntPtr"},
    objectType{"UnityEngine.Object"}, cameraType{"UnityEngine.Camera"}, safetyType{"Unity.Collections.LowLevel.Unsafe.AtomicSafetyHandle"};
struct Class;
struct Method {
    std::string name;
    std::vector<Type*> parameters;
    Type* result = &voidType;
    bool isStatic = true, generic = false;
    Class* owner = nullptr;
    void* entry = nullptr;
};
struct Class { std::string namespc, name; std::vector<Method*> methods; Class* parent = nullptr; };
struct Image { std::string name; std::vector<Class*> classes; };
struct Patch {
    dobby_dummy_func_t replacement = nullptr;
    int result = 0;
    unsigned calls = 0;
    bool returnOriginal = true, installReplacement = true, executable = true;
} srpPatch, cameraPatch;
struct SrpCall { void* pipe; intptr_t loop; void* request; const void* method; };
struct CameraCall { void* camera; const void* method; };
std::vector<std::string> order;
std::vector<SrpCall> srpCalls;
std::vector<CameraCall> cameraCalls;
std::string missingApi;
bool attached = true, throwOriginal = false, testPublication = false;
unsigned callbackCalls = 0;
std::function<void()> callbackAction;
std::future<void> publicationCall;
int pipelineObject = 1, requestObject = 2, cameraObject = 3, metadataObject = 4;
const intptr_t loopValue = -static_cast<intptr_t>(0x123456789abLL);
struct OriginalFailure {};

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class T> void* address(T pointer) { return reinterpret_cast<void*>(pointer); }
void srpEntry(void*, intptr_t, void*, const void*) { require(false, "SRP entry is not its trampoline"); }
void cameraEntry(void*, const void*) { require(false, "Camera entry is not its trampoline"); }
void originalSrp(void* pipe, intptr_t loop, void* request, const void* method) {
    order.push_back("srp"); srpCalls.push_back({pipe, loop, request, method});
    if (throwOriginal) throw OriginalFailure{};
}
void originalCamera(void* camera, const void* method) {
    order.push_back("camera"); cameraCalls.push_back({camera, method});
    if (throwOriginal) throw OriginalFailure{};
}
void invokeSrp(void* pipe, intptr_t loop, void* request, const void* method) {
    require(srpPatch.replacement != nullptr, "SRP replacement must be installed");
    reinterpret_cast<Srp>(srpPatch.replacement)(pipe, loop, request, method);
}
void invokeCamera(void* camera, const void* method) {
    require(cameraPatch.replacement != nullptr, "Camera replacement must be installed");
    reinterpret_cast<Camera>(cameraPatch.replacement)(camera, method);
}
Class srpClass{"UnityEngine.Rendering", "RenderPipelineManager", {}}, cameraClass{"UnityEngine", "Camera", {}};
Method srpMethod{"DoRenderLoop_Internal", {&assetType, &pointerType, &objectType}, &voidType, true, false, &srpClass, address(srpEntry)};
Method cameraMethod{"FireOnPreCull", {&cameraType}, &voidType, true, false, &cameraClass, address(cameraEntry)};
Image core{"UnityEngine.CoreModule.dll", {&srpClass, &cameraClass}}, wrongImage{"Other.dll", {&srpClass, &cameraClass}};

const char* imageName(void* image) { return static_cast<Image*>(image)->name.c_str(); }
void* classFromName(void* image, const char* namespc, const char* name) {
    for (auto klass : static_cast<Image*>(image)->classes)
        if (klass->namespc == namespc && klass->name == name) return klass;
    return nullptr;
}
void* classMethods(void* klass, void** iterator) {
    const auto index = reinterpret_cast<uintptr_t>(*iterator);
    auto& methods = static_cast<Class*>(klass)->methods;
    if (index >= methods.size()) return nullptr;
    *iterator = reinterpret_cast<void*>(index + 1);
    return methods[index];
}
void* classParent(void* klass) { return static_cast<Class*>(klass)->parent; }
const char* methodName(void* method) { return static_cast<Method*>(method)->name.c_str(); }
uint32_t methodCount(void* method) { return static_cast<uint32_t>(static_cast<Method*>(method)->parameters.size()); }
void* methodParameter(void* method, uint32_t index) { return static_cast<Method*>(method)->parameters.at(index); }
void* methodResult(void* method) { return static_cast<Method*>(method)->result; }
uint32_t methodFlags(void* method, uint32_t*) { return static_cast<Method*>(method)->isStatic ? 0x10u : 0u; }
bool methodGeneric(void* method) { return static_cast<Method*>(method)->generic; }
void* methodClass(void* method) { return static_cast<Method*>(method)->owner; }
char* typeName(void* type) {
    const auto& name = static_cast<Type*>(type)->name;
    auto copy = static_cast<char*>(std::malloc(name.size() + 1));
    require(copy != nullptr, "Type-name allocation failed");
    std::memcpy(copy, name.c_str(), name.size() + 1); return copy;
}
void* threadCurrent() { return attached ? &core : nullptr; }
void unusedExport() { require(false, "Frame installation must not invoke unrelated Unity operations"); }
void callback() {
    require(srpPatch.calls == 1 && cameraPatch.calls == 1, "Callback must wait until both hook attempts finish");
    ++callbackCalls; order.push_back("callback");
    if (callbackAction) callbackAction();
}
void expectOrder(std::initializer_list<const char*> expected) {
    std::vector<std::string> values;
    for (auto value : expected) values.emplace_back(value);
    if (order != values) { for (const auto& item : order) std::cerr << item << ' '; std::cerr << '\n'; }
    require(order == values, "Callback must precede the correct original, without callback recursion");
}
void clearCalls() { order.clear(); srpCalls.clear(); cameraCalls.clear(); }
template<class F> void mustThrow(F action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}
void expectRejected() {
    mustThrow([] { ft::il2cpp::frame_hook::install({&core}, callback); }, "Invalid live metadata must reject installation");
    require(srpPatch.calls == 0 && cameraPatch.calls == 0, "Validation must finish before any native patch");
}
}

namespace ft::il2cpp {
void* lookup(const char* name) {
    if (missingApi == name) return nullptr;
    const std::map<std::string, void*> implemented{
        {"il2cpp_image_get_name", address(imageName)}, {"il2cpp_method_get_class", address(methodClass)},
        {"il2cpp_thread_current", address(threadCurrent)}, {"il2cpp_class_from_name", address(classFromName)},
        {"il2cpp_class_get_methods", address(classMethods)}, {"il2cpp_class_get_parent", address(classParent)},
        {"il2cpp_method_get_name", address(methodName)}, {"il2cpp_method_get_param_count", address(methodCount)},
        {"il2cpp_method_get_param", address(methodParameter)}, {"il2cpp_method_get_return_type", address(methodResult)},
        {"il2cpp_method_get_flags", address(methodFlags)}, {"il2cpp_method_is_generic", address(methodGeneric)},
        {"il2cpp_type_get_name", address(typeName)}, {"il2cpp_free", address(std::free)}
    };
    const auto found = implemented.find(name);
    if (found != implemented.end()) return found->second;
    for (const char* unused : {"class_get_type", "type_get_object", "object_new", "object_unbox", "class_value_size",
            "array_new", "array_length", "array_element_size", "array_object_header_size", "object_get_class",
            "string_new", "string_length", "string_chars", "gchandle_new", "gchandle_get_target", "gchandle_free", "gc_wbarrier_set_field"})
        if (std::string("il2cpp_") + unused == name) return address(unusedExport);
    return nullptr;
}
void* methodPointer(const void* method) { return static_cast<const Method*>(method)->entry; }
bool executable(uintptr_t entryAddress, size_t bytes) {
    require(bytes == 16, "Entry-point executable range must be checked");
    if (entryAddress == reinterpret_cast<uintptr_t>(address(srpEntry))) return srpPatch.executable;
    if (entryAddress == reinterpret_cast<uintptr_t>(address(cameraEntry))) return cameraPatch.executable;
    return false;
}
}
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" int DobbyHook(void* entryAddress, dobby_dummy_func_t replacement, dobby_dummy_func_t* original) {
    const bool srp = entryAddress == address(srpEntry);
    require(srp || entryAddress == address(cameraEntry), "Dobby address must come from live metadata");
    auto& patch = srp ? srpPatch : cameraPatch;
    require(++patch.calls == 1, "Native entry must never be patched twice");
    if (!srp && testPublication) {
        // Native calls can arrive after a patch is visible but before Dobby
        // returns. Production's installation mutex must hold this caller until
        // both original pointers and the ready flag have been published.
        std::promise<void> started;
        auto startedFuture = started.get_future();
        publicationCall = std::async(std::launch::async, [signal = std::move(started)]() mutable {
            signal.set_value();
            invokeSrp(&pipelineObject, loopValue, &requestObject, nullptr);
        });
        startedFuture.get();
        require(publicationCall.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
            "Render call must wait for trampoline publication while installation holds its mutex");
    }
    *original = patch.returnOriginal ? (srp ? reinterpret_cast<dobby_dummy_func_t>(originalSrp) :
        reinterpret_cast<dobby_dummy_func_t>(originalCamera)) : nullptr;
    // Write visibility is independent of the returned original. Real Dobby
    // tests separately prove that missing relocation prevents this write.
    if (patch.installReplacement) patch.replacement = replacement;
    return patch.result;
}

int main(int argc, char** argv) {
    require(argc == 2, "Provide success or a failure scenario");
    const std::string scenario = argv[1];
    srpClass.methods = {&srpMethod}; cameraClass.methods = {&cameraMethod};
    if (scenario != "success") {
        const bool second = scenario.rfind("second_", 0) == 0;
        auto& failed = second ? cameraPatch : srpPatch;
        if (scenario.ends_with("error_null")) {
            failed.result = -1; failed.returnOriginal = false; failed.installReplacement = false;
        } else if (scenario.ends_with("commit_prewrite_error")) {
            failed.result = -1; failed.installReplacement = false;
        } else if (scenario.ends_with("commit_postwrite_error")) failed.result = -1;
        else require(false, "Unknown failure scenario");
        testPublication = second;
        mustThrow([] { ft::il2cpp::frame_hook::install({&core}, callback); }, "Native failure must throw");
        if (publicationCall.valid()) publicationCall.get();
        require(callbackCalls == 0, "Half-installed or failed hooks must not run callbacks");
        clearCalls();
        if (srpPatch.replacement) invokeSrp(&pipelineObject, loopValue, &requestObject, nullptr);
        if (cameraPatch.replacement) invokeCamera(&cameraObject, nullptr);
        require(callbackCalls == 0, "Failure must leave callback publication permanently disabled");
        if (srpPatch.replacement) require(srpCalls.size() == 1 && srpCalls[0].pipe == &pipelineObject &&
            srpCalls[0].loop == loopValue && srpCalls[0].request == &requestObject && srpCalls[0].method == nullptr,
            "Already patched SRP must keep forwarding its full ABI after another hook fails");
        if (cameraPatch.replacement) require(cameraCalls.size() == 1 && cameraCalls[0].camera == &cameraObject && cameraCalls[0].method == nullptr,
            "A returned camera trampoline must keep forwarding while callbacks are disabled");
        mustThrow([] { ft::il2cpp::frame_hook::install({&core}, callback); }, "Failed native installation cannot be retried");
        require(srpPatch.calls == 1 && cameraPatch.calls == (second ? 1u : 0u), "Failure retry must not repatch either entry");
        std::cout << "frame_hook_test: " << scenario << " passed\n";
        return 0;
    }

    // Each validation failure is recoverable because no native patch occurred.
    mustThrow([] { ft::il2cpp::frame_hook::install({&core}, {}); }, "Empty callback must fail");
    mustThrow([] { ft::il2cpp::frame_hook::install({&wrongImage}, callback); }, "Matching class names in a wrong assembly must fail");
    auto copyCore = core;
    mustThrow([&] { ft::il2cpp::frame_hook::install({&core, &copyCore}, callback); }, "Ambiguous CoreModule images must fail");
    core.classes.clear();
    mustThrow([] { ft::il2cpp::frame_hook::install({&core, &wrongImage}, callback); }, "Resolver must not fall back to classes from another assembly");
    core.classes = {&srpClass, &cameraClass};
    missingApi = "il2cpp_method_get_class"; expectRejected(); missingApi.clear();
    attached = false; expectRejected(); attached = true;
    const auto correctSrp = srpMethod, correctCamera = cameraMethod;
    srpMethod.parameters.push_back(&safetyType); expectRejected(); srpMethod = correctSrp;
    srpMethod.parameters[2] = &intType; expectRejected(); srpMethod = correctSrp;
    srpMethod.isStatic = false; expectRejected(); srpMethod = correctSrp;
    srpMethod.result = &intType; expectRejected(); srpMethod = correctSrp;
    srpMethod.generic = true; expectRejected(); srpMethod = correctSrp;
    srpMethod.owner = &cameraClass; expectRejected(); srpMethod = correctSrp;
    cameraMethod.parameters.clear(); expectRejected(); cameraMethod = correctCamera;
    cameraMethod.isStatic = false; expectRejected(); cameraMethod = correctCamera;
    cameraMethod.result = &intType; expectRejected(); cameraMethod = correctCamera;
    auto duplicate = srpMethod; srpClass.methods.push_back(&duplicate); expectRejected(); srpClass.methods.pop_back();
    cameraMethod.entry = srpMethod.entry; expectRejected(); cameraMethod = correctCamera;
    cameraMethod.entry = nullptr; expectRejected(); cameraMethod = correctCamera;
    srpPatch.executable = false; expectRejected(); srpPatch.executable = true;
    require(srpPatch.calls == 0 && cameraPatch.calls == 0, "No rejected signature may reach Dobby");

    testPublication = true;
    ft::il2cpp::frame_hook::install({nullptr, &wrongImage, &core, &core}, callback);
    publicationCall.get();
    expectOrder({"callback", "srp"});
    require(callbackCalls == 1 && srpPatch.calls == 1 && cameraPatch.calls == 1,
        "Callback may run only once the complete installation is ready");
    clearCalls(); invokeSrp(&pipelineObject, loopValue, &requestObject, &metadataObject);
    expectOrder({"callback", "srp"});
    require(srpCalls.size() == 1 && srpCalls[0].pipe == &pipelineObject && srpCalls[0].loop == loopValue &&
        srpCalls[0].request == &requestObject && srpCalls[0].method == &metadataObject, "SRP must forward all four ABI arguments exactly");
    clearCalls(); invokeCamera(&cameraObject, &metadataObject);
    expectOrder({"callback", "camera"});
    require(cameraCalls.size() == 1 && cameraCalls[0].camera == &cameraObject && cameraCalls[0].method == &metadataObject,
        "Camera must use its separate two-argument trampoline");
    clearCalls(); invokeSrp(nullptr, 0, nullptr, nullptr); invokeCamera(nullptr, nullptr);
    expectOrder({"callback", "srp", "callback", "camera"});
    require(srpCalls[0].pipe == nullptr && srpCalls[0].loop == 0 && srpCalls[0].request == nullptr &&
        srpCalls[0].method == nullptr && cameraCalls[0].camera == nullptr && cameraCalls[0].method == nullptr,
        "Null MethodInfo and null arguments must forward unchanged");

    clearCalls(); const auto countBeforeReentry = callbackCalls;
    callbackAction = [] { invokeCamera(&cameraObject, nullptr); invokeSrp(nullptr, loopValue, nullptr, nullptr); };
    invokeSrp(&pipelineObject, loopValue, &requestObject, &metadataObject);
    expectOrder({"callback", "camera", "srp", "srp"});
    require(callbackCalls == countBeforeReentry + 1 && srpCalls.size() == 2 && cameraCalls.size() == 1,
        "Callback reentry through either ABI must still call every native original once");
    clearCalls(); callbackAction = [] { throw std::runtime_error("callback"); };
    invokeSrp(&pipelineObject, loopValue, &requestObject, nullptr); expectOrder({"callback", "srp"});
    clearCalls(); callbackAction = [] { throw 7; };
    invokeCamera(&cameraObject, nullptr); expectOrder({"callback", "camera"});
    callbackAction = {}; throwOriginal = true;
    for (bool srp : {true, false}) {
        clearCalls(); bool propagated = false;
        try {
            if (srp) invokeSrp(nullptr, loopValue, nullptr, nullptr); else invokeCamera(nullptr, nullptr);
        } catch (const OriginalFailure&) { propagated = true; }
        require(propagated, "Native original exception must propagate unchanged");
        require(order.size() == 2 && order[0] == "callback", "Callback must precede even a throwing original");
    }
    throwOriginal = false;
    ft::il2cpp::frame_hook::install({&core}, [] { require(false, "Successful install must not replace an active callback"); });
    clearCalls(); invokeCamera(&cameraObject, nullptr); expectOrder({"callback", "camera"});
    require(srpPatch.calls == 1 && cameraPatch.calls == 1, "Repeated install must not patch either live address again");
    std::cout << "frame_hook_test: success passed\n";
}
