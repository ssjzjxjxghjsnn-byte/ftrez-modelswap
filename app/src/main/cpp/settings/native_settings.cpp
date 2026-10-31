#include "settings/native_settings.h"
#include "settings/model_selection.h"
#include "self_model/model_runtime.h"
#include "self_model/unity_api.h"
#include "il2cpp/il2cpp_api.h"
#include "sdk/model_bindings.h"
#include "dobby.h"
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace ft::settings {
namespace {
using Clock = std::chrono::steady_clock;
using UnityApi = self_model::UnityApi;
struct V2 { float x, y; };
struct V3 { float x, y, z; };
struct Rect { float x, y, width, height; };
constexpr const char* RowName = "FTRezModelSelection";

void log(const char* value) { __android_log_print(ANDROID_LOG_INFO, "FTREZ-SETTINGS", "%s", value); }

struct State;
State& state();
using ButtonCallback = void (*)(void*, void*, const void*);
ButtonCallback originalPointer = nullptr, originalSubmit = nullptr;
void pointerClick(void*, void*, const void*);
void submit(void*, void*, const void*);

struct State {
    UnityApi api;
    SelfModelControls controls;
    std::atomic<bool> installed{false};
    bool nativeAttempted = false;
    std::mutex installMutex, saveMutex;
    std::string path, savedBytes;
    bool loaded = false;
    ModelSelection selection;
    std::vector<SelectionChoice> choices;
    Clock::time_point nextCatalog{}, nextSearch{}, nextRefresh{}, nextError{};
    uint32_t owner = 0, donor = 0, row = 0, title = 0, previous = 0, next = 0;
    std::atomic<void*> previousPointer{nullptr}, nextPointer{nullptr};
    std::string shownTitle;
    struct LayoutEdit { uint32_t target; V2 beforeSize, beforeAnchor, afterSize, afterAnchor; };
    std::vector<LayoutEdit> layoutEdits;
    void *graphicsField = nullptr, *donorField = nullptr;
    void* (*getField)(void*, const char*) = nullptr;
    void (*readField)(void*, void*, void*) = nullptr;
    uint32_t (*weakHandle)(void*, bool) = nullptr;
    void *findObjects = nullptr, *alive = nullptr, *componentGO = nullptr, *componentTF = nullptr;
    void *getComponent = nullptr, *childrenComponents = nullptr, *active = nullptr, *setActive = nullptr;
    void *referenceGet = nullptr, *instantiate = nullptr, *destroy = nullptr, *getParent = nullptr;
    void *getChildCount = nullptr, *getChild = nullptr, *findChild = nullptr, *setName = nullptr;
    void *lastSibling = nullptr, *localPosition = nullptr, *setLocalPosition = nullptr;
    void *worldPosition = nullptr, *setWorldPosition = nullptr, *rect = nullptr;
    void *getSize = nullptr, *setSize = nullptr, *getPivot = nullptr, *setPivot = nullptr;
    void *getAnchored = nullptr, *setAnchored = nullptr, *setAnchorMin = nullptr, *setAnchorMax = nullptr;
    void *setOffsetMin = nullptr, *setOffsetMax = nullptr, *setText = nullptr, *setAlignment = nullptr;
    void *setBestFit = nullptr, *setMinFont = nullptr, *setMaxFont = nullptr, *getFont = nullptr;
    void *getScrollContent = nullptr, *isChildOf = nullptr, *rebuildLayout = nullptr, *getVerticalFit = nullptr;
    void *getBehaviourEnabled = nullptr, *setBehaviourEnabled = nullptr, *getPointerButton = nullptr, *isInteractable = nullptr;
    void *setClick = nullptr, *clickCtor = nullptr;

    void addClass(const std::vector<void*>& images, const char* full) {
        std::string name(full), ns;
        const auto dot = name.rfind('.');
        if (dot != name.npos) { ns = name.substr(0, dot); name.erase(0, dot + 1); }
        void* found = nullptr;
        for (auto image : images) if (auto k = api.class_from_name(image, ns.c_str(), name.c_str())) {
            if (found && found != k) throw std::runtime_error(std::string("Ambiguous class: ") + full);
            found = k;
        }
        if (!found) throw std::runtime_error(std::string("Missing class: ") + full);
        api.classes[full] = found;
    }
    void bind(const std::vector<void*>& images) {
        const auto& bindings = sdk::settingsBindings();
        sdk::validateBindings(images, bindings);
        api.bind(images);
        auto imageName = reinterpret_cast<const char* (*)(void*)>(unity::resolve_api("il2cpp_image_get_name"));
        if (!imageName) throw std::runtime_error("Image metadata unavailable");
        for (const auto& [key, symbol] : bindings.types) {
            void* found = nullptr;
            for (auto image : images) if (auto name = imageName(image); name && symbol.assembly == name) {
                if (auto klass = api.class_from_name(image, symbol.namespc.c_str(), symbol.type.c_str())) {
                    if (found && found != klass) throw std::runtime_error("Ambiguous settings class: " + key);
                    found = klass;
                }
            }
            if (!found) throw std::runtime_error("Missing configured settings class: " + key);
            api.classes[key] = found;
        }
        for (auto name : {"UnityEngine.UI.Button",
                "UnityEngine.UI.Text", "UnityEngine.RectTransform", "UnityEngine.Behaviour",
                "UnityEngine.UI.LayoutGroup", "UnityEngine.UI.LayoutRebuilder", "UnityEngine.UI.ScrollRect", "UnityEngine.UI.ContentSizeFitter",
                "UnityEngine.EventSystems.PointerEventData"}) addClass(images, name);
        // Query nested type spellings from metadata; reflection and dump spelling
        // of the same nested type can differ across Unity releases.
        auto nested = reinterpret_cast<void* (*)(void*, void**)>(unity::resolve_api("il2cpp_class_get_nested_types"));
        auto className = reinterpret_cast<const char* (*)(void*)>(unity::resolve_api("il2cpp_class_get_name"));
        if (!nested || !className) throw std::runtime_error("Nested type metadata unavailable");
        for (const auto& entry : {std::array<const char*, 3>{"UnityEngine.UI.Button", "ButtonClickedEvent", "ButtonEvent"},
                std::array<const char*, 3>{"UnityEngine.EventSystems.PointerEventData", "InputButton", "PointerButton"},
                std::array<const char*, 3>{"UnityEngine.UI.ContentSizeFitter", "FitMode", "FitMode"}}) {
            void* it = nullptr;
            while (auto k = nested(api.klass(entry[0]), &it))
                if (!std::strcmp(className(k), entry[1])) api.classes[entry[2]] = k;
            api.klass(entry[2]);
        }
#define BIND_API(n, symbol) n = reinterpret_cast<decltype(n)>(unity::resolve_api(symbol)); if (!n) throw std::runtime_error(symbol)
        BIND_API(getField, "il2cpp_class_get_field_from_name");
        BIND_API(readField, "il2cpp_field_get_value");
        BIND_API(weakHandle, "il2cpp_gchandle_new_weakref");
#undef BIND_API
        graphicsField = getField(api.klass("SettingsUI"), bindings.fields.at("graphicsReference").member.c_str());
        donorField = getField(api.klass("SettingsUI"), bindings.fields.at("donorRow").member.c_str());
        if (!graphicsField || !donorField) throw std::runtime_error("Configured settings fields unavailable");
        findObjects = api.method("UnityEngine.Object", "FindObjectsOfType", {"System.Type"}, "UnityEngine.Object[]", true);
        alive = api.method("UnityEngine.Object", "op_Implicit", {"UnityEngine.Object"}, "System.Boolean", true);
        componentGO = api.method("UnityEngine.Component", "get_gameObject", {}, "UnityEngine.GameObject");
        componentTF = api.method("UnityEngine.Component", "get_transform", {}, "UnityEngine.Transform");
        getComponent = api.method("UnityEngine.GameObject", "GetComponent", {"System.Type"}, "UnityEngine.Component");
        childrenComponents = api.method("UnityEngine.Component", "GetComponentsInChildren", {"System.Type", "System.Boolean"}, "UnityEngine.Component[]");
        active = api.method("UnityEngine.GameObject", "get_activeInHierarchy", {}, "System.Boolean");
        setActive = api.method("UnityEngine.GameObject", "SetActive", {"System.Boolean"}, "System.Void");
        const auto& getReference = bindings.methods.at("referenceGet");
        referenceGet = api.method("UIReference", getReference.member.c_str(), {"System.Int32"}, "UnityEngine.GameObject");
        instantiate = api.method("UnityEngine.Object", "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform", "System.Boolean"}, "UnityEngine.Object", true);
        destroy = api.method("UnityEngine.Object", "Destroy", {"UnityEngine.Object"}, "System.Void", true);
        getParent = api.method("UnityEngine.Transform", "get_parent", {}, "UnityEngine.Transform");
        getChildCount = api.method("UnityEngine.Transform", "get_childCount", {}, "System.Int32");
        getChild = api.method("UnityEngine.Transform", "GetChild", {"System.Int32"}, "UnityEngine.Transform");
        findChild = api.method("UnityEngine.Transform", "Find", {"System.String"}, "UnityEngine.Transform");
        setName = api.method("UnityEngine.Object", "set_name", {"System.String"}, "System.Void");
        lastSibling = api.method("UnityEngine.Transform", "SetAsLastSibling", {}, "System.Void");
        localPosition = api.method("UnityEngine.Transform", "get_localPosition", {}, "UnityEngine.Vector3");
        setLocalPosition = api.method("UnityEngine.Transform", "set_localPosition", {"UnityEngine.Vector3"}, "System.Void");
        worldPosition = api.method("UnityEngine.Transform", "get_position", {}, "UnityEngine.Vector3");
        setWorldPosition = api.method("UnityEngine.Transform", "set_position", {"UnityEngine.Vector3"}, "System.Void");
        rect = api.method("UnityEngine.RectTransform", "get_rect", {}, "UnityEngine.Rect");
        getSize = api.method("UnityEngine.RectTransform", "get_sizeDelta", {}, "UnityEngine.Vector2");
        setSize = api.method("UnityEngine.RectTransform", "set_sizeDelta", {"UnityEngine.Vector2"}, "System.Void");
        getPivot = api.method("UnityEngine.RectTransform", "get_pivot", {}, "UnityEngine.Vector2");
        setPivot = api.method("UnityEngine.RectTransform", "set_pivot", {"UnityEngine.Vector2"}, "System.Void");
        getAnchored = api.method("UnityEngine.RectTransform", "get_anchoredPosition", {}, "UnityEngine.Vector2");
        setAnchored = api.method("UnityEngine.RectTransform", "set_anchoredPosition", {"UnityEngine.Vector2"}, "System.Void");
        setAnchorMin = api.method("UnityEngine.RectTransform", "set_anchorMin", {"UnityEngine.Vector2"}, "System.Void");
        setAnchorMax = api.method("UnityEngine.RectTransform", "set_anchorMax", {"UnityEngine.Vector2"}, "System.Void");
        setOffsetMin = api.method("UnityEngine.RectTransform", "set_offsetMin", {"UnityEngine.Vector2"}, "System.Void");
        setOffsetMax = api.method("UnityEngine.RectTransform", "set_offsetMax", {"UnityEngine.Vector2"}, "System.Void");
        setText = api.method("UnityEngine.UI.Text", "set_text", {"System.String"}, "System.Void");
        setAlignment = api.method("UnityEngine.UI.Text", "set_alignment", {"UnityEngine.TextAnchor"}, "System.Void");
        setBestFit = api.method("UnityEngine.UI.Text", "set_resizeTextForBestFit", {"System.Boolean"}, "System.Void");
        setMinFont = api.method("UnityEngine.UI.Text", "set_resizeTextMinSize", {"System.Int32"}, "System.Void");
        setMaxFont = api.method("UnityEngine.UI.Text", "set_resizeTextMaxSize", {"System.Int32"}, "System.Void");
        getFont = api.method("UnityEngine.UI.Text", "get_fontSize", {}, "System.Int32");
        getScrollContent = api.method("UnityEngine.UI.ScrollRect", "get_content", {}, "UnityEngine.RectTransform");
        isChildOf = api.method("UnityEngine.Transform", "IsChildOf", {"UnityEngine.Transform"}, "System.Boolean");
        rebuildLayout = api.method("UnityEngine.UI.LayoutRebuilder", "ForceRebuildLayoutImmediate", {"UnityEngine.RectTransform"}, "System.Void", true);
        getBehaviourEnabled = api.method("UnityEngine.Behaviour", "get_isActiveAndEnabled", {}, "System.Boolean");
        setBehaviourEnabled = api.method("UnityEngine.Behaviour", "set_enabled", {"System.Boolean"}, "System.Void");
        const auto fitModeName = api.typeName(api.class_get_type(api.klass("FitMode")));
        getVerticalFit = api.method("UnityEngine.UI.ContentSizeFitter", "get_verticalFit", {}, fitModeName.c_str());
        const auto pointerButtonName = api.typeName(api.class_get_type(api.klass("PointerButton")));
        getPointerButton = api.method("UnityEngine.EventSystems.PointerEventData", "get_button", {}, pointerButtonName.c_str());
        isInteractable = api.method("UnityEngine.UI.Button", "IsInteractable", {}, "System.Boolean");
        // Resolve set_onClick with the actual nested type spelling in metadata.
        auto eventName = api.typeName(api.class_get_type(api.klass("ButtonEvent")));
        setClick = api.method("UnityEngine.UI.Button", "set_onClick", {eventName.c_str()}, "System.Void");
        clickCtor = api.method("ButtonEvent", ".ctor", {}, "System.Void");
        controls = self_model::uiControls();
    }

    void* ref(uint32_t handle) { return api.hold(handle ? api.gchandle_get_target(handle) : nullptr); }
    bool live(void* object) { return object && api.value<bool>(alive, nullptr, {object}); }
    bool activeComponent(void* object) { return live(object) && api.value<bool>(active, api.call(componentGO, object)); }
    void* read(void* object, void* f) { void* v = nullptr; readField(object, f, &v); return api.hold(v); }
    void* component(void* go, const char* type) { return api.call(getComponent, go, {api.type(type)}); }
    void* at(void* reference, int index) { return api.call(referenceGet, reference, {&index}); }
    uint32_t weak(void* value) {
        auto handle = value ? weakHandle(value, false) : 0;
        if (value && !handle) throw std::runtime_error("Cannot retain settings control");
        return handle;
    }
    void release(uint32_t& value) { if (value) api.gchandle_free(value); value = 0; }
    void clearRow(bool remove) {
        previousPointer.store(nullptr); nextPointer.store(nullptr);
        auto old = ref(row);
        if (remove && live(old)) { bool off = false; api.call(setActive, old, {&off}); api.call(destroy, nullptr, {old}); }
        undoLayout();
        for (auto p : {&donor, &row, &title, &previous, &next}) release(*p);
        shownTitle.clear();
    }
    void refreshCatalog() {
        if (!loaded) {
            std::lock_guard lock(saveMutex);
            if (!savedBytes.empty()) selection.decode(savedBytes);
            loaded = true;
        }
        if (!controls.catalog || !controls.read || !controls.write) return;
        choices.clear();
        std::set<std::string> ids;
        for (const auto& c : controls.catalog()) if (c.key >= 0 && !c.id.empty() && ModelSelection::validId(c.id) && ids.insert(c.id).second)
            choices.push_back({c.key, c.id, c.label});
        applySelection();
    }
    void applySelection() {
        const auto key = selection.resolve(choices);
        if (!key || !controls.read || !controls.write) return;
        auto options = controls.read(), original = options;
        options.enabled = *key >= 0;
        if (options.enabled) options.model = *key;
        options.hideOriginal = true;
        options.lobbyRendering = true;
        options.rabbitEnabled = false;
        if (!(options == original)) controls.write(options);
    }
    void save() {
        std::lock_guard lock(saveMutex);
        if (path.empty()) return;
        const auto bytes = selection.encode(), temporary = path + ".tmp";
        auto file = std::fopen(temporary.c_str(), "wb");
        if (!file) { log("Cannot open model-selection temporary file"); return; }
        bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        if (ok) ok = std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
        if (std::fclose(file) != 0) ok = false;
        if (ok) ok = std::rename(temporary.c_str(), path.c_str()) == 0;
        if (!ok) { std::remove(temporary.c_str()); log("Cannot persist model selection"); }
    }
    void updateTitle() {
        auto text = ref(title);
        if (!live(text)) return;
        const auto label = std::string("修改角色模型（") + selection.label(choices) + "）";
        if (label != shownTitle) { api.call(setText, text, {api.text(label)}); shownTitle = label; }
    }
    bool click(void* button, void* eventData, bool pointer) {
        int direction = button == previousPointer.load() ? -1 : button == nextPointer.load() ? 1 : 0;
        if (!button || !direction) return false;
        if (button != ref(direction < 0 ? previous : next)) return false;
        // Always consume an owned control, even on failure: the donor's original
        // quality-setting listeners must never run for a model choice.
        if (!activeComponent(button) || !api.value<bool>(isInteractable, button)) return true;
        if (pointer && (!eventData || api.value<int>(getPointerButton, eventData) != 0)) return true;
        refreshCatalog();
        selection.cycle(choices, direction);
        applySelection(); updateTitle(); save();
        return true;
    }

    void setVector(void* method, void* transform, V2 v) { api.call(method, transform, {&v}); }
    void setupText(void* text, const std::string& value, bool rowTitle) {
        api.call(setText, text, {api.text(value)});
        bool fit = true;
        int min = 16, max = std::max(20, api.value<int>(getFont, text));
        int alignment = rowTitle ? 3 : 4; // MiddleLeft / MiddleCenter.
        api.call(setAlignment, text, {&alignment});
        api.call(setBestFit, text, {&fit});
        api.call(setMinFont, text, {&min}); api.call(setMaxFont, text, {&max});
        if (rowTitle) {
            auto transform = api.call(componentTF, text);
            setVector(setAnchorMin, transform, {0, 0}); setVector(setAnchorMax, transform, {1, 1});
            setVector(setOffsetMin, transform, {18, 0}); setVector(setOffsetMax, transform, {-254, 0});
        }
    }
    void* makeButton(void* source, void* parent, const char* name, const char* label, float x, float height) {
        bool world = false;
        auto go = api.call(instantiate, nullptr, {source, parent, &world});
        api.call(setName, go, {api.text(name)});
        auto button = component(go, "UnityEngine.UI.Button");
        if (!button || api.object_get_class(button) != api.klass("UnityEngine.UI.Button"))
            throw std::runtime_error("Frame button is not a standard Unity button");
        // Replace both persistent and runtime listeners, rather than only clearing
        // AddListener callbacks. Native OnPointerClick/OnSubmit own the new action.
        auto event = api.hold(api.object_new(api.klass("ButtonEvent")));
        api.call(clickCtor, event); api.call(setClick, button, {event});
        auto transform = api.call(componentTF, button);
        setVector(setAnchorMin, transform, {1, .5f}); setVector(setAnchorMax, transform, {1, .5f});
        setVector(setPivot, transform, {1, .5f}); setVector(setSize, transform, {108, height});
        setVector(setAnchored, transform, {x, 0});
        bool includeInactive = true;
        auto texts = api.elements<void*>(api.call(childrenComponents, button, {api.type("UnityEngine.UI.Text"), &includeInactive}), 32);
        if (texts.empty()) throw std::runtime_error("Frame button label missing");
        setupText(texts.front(), label, false);
        if (auto selected = api.call(findChild, transform, {api.text("Choose")})) {
            bool off = false; api.call(setActive, api.call(componentGO, selected), {&off});
        }
        bool on = true; api.call(setActive, go, {&on});
        return button;
    }
    std::vector<void*> children(void* parent) {
        const auto count = api.value<int>(getChildCount, parent);
        if (count < 0 || count > 512) throw std::runtime_error("Unexpected settings hierarchy size");
        std::vector<void*> result;
        for (int i = 0; i < count; ++i) result.push_back(api.call(getChild, parent, {&i}));
        return result;
    }
    bool isRect(void* object) { return object && api.object_get_class(object) == api.klass("UnityEngine.RectTransform"); }
    void* layout(void* transform) {
        auto value = component(api.call(componentGO, transform), "UnityEngine.UI.LayoutGroup");
        return value && api.value<bool>(getBehaviourEnabled, value) ? value : nullptr;
    }
    bool autoHeight(void* transform) {
        auto fitter = component(api.call(componentGO, transform), "UnityEngine.UI.ContentSizeFitter");
        return fitter && api.value<bool>(getBehaviourEnabled, fitter) && api.value<int>(getVerticalFit, fitter) != 0;
    }
    float bottom(void* parent, void* except = nullptr) {
        float result = 0;
        bool found = false;
        for (auto child : children(parent)) if (child != except && isRect(child) && activeComponent(child)) {
            const auto r = api.value<Rect>(rect, child);
            const auto p = api.value<V3>(localPosition, child);
            if (!std::isfinite(r.height) || !std::isfinite(p.y)) continue;
            const float low = p.y + r.y;
            if (!found || low < result) result = low;
            found = true;
        }
        return result;
    }
    void growDown(void* transform, float delta) {
        if (delta <= .5f || !isRect(transform)) return;
        if (!std::isfinite(delta) || delta > 1500) throw std::runtime_error("Invalid settings layout extent");
        // Preserve original controls in world space even with centered anchors.
        std::vector<std::pair<void*, V3>> positions;
        for (auto child : children(transform)) positions.emplace_back(child, api.value<V3>(worldPosition, child));
        auto size = api.value<V2>(getSize, transform);
        auto pivot = api.value<V2>(getPivot, transform);
        auto anchor = api.value<V2>(getAnchored, transform);
        LayoutEdit* edit = nullptr;
        for (auto& entry : layoutEdits) if (ref(entry.target) == transform) { edit = &entry; break; }
        if (!edit) {
            layoutEdits.push_back({weak(transform), size, anchor, size, anchor});
            edit = &layoutEdits.back();
        }
        size.y += delta; anchor.y -= (1 - pivot.y) * delta;
        setVector(setSize, transform, size); edit->afterSize = size;
        setVector(setAnchored, transform, anchor); edit->afterAnchor = anchor;
        for (auto& [child, position] : positions) api.call(setWorldPosition, child, {&position});
    }
    void undoLayout() {
        // Undo only our own dimensions. If the game has since recalculated an
        // ancestor, its newer dimensions take precedence.
        auto same = [](V2 a, V2 b) { return std::abs(a.x - b.x) < .5f && std::abs(a.y - b.y) < .5f; };
        for (auto i = layoutEdits.rbegin(); i != layoutEdits.rend(); ++i) {
            try {
                auto transform = ref(i->target);
                if (live(transform) && same(api.value<V2>(getSize, transform), i->afterSize) &&
                        same(api.value<V2>(getAnchored, transform), i->afterAnchor)) {
                    std::vector<std::pair<void*, V3>> positions;
                    for (auto child : children(transform)) positions.emplace_back(child, api.value<V3>(worldPosition, child));
                    setVector(setSize, transform, i->beforeSize);
                    setVector(setAnchored, transform, i->beforeAnchor);
                    for (auto& [child, position] : positions) api.call(setWorldPosition, child, {&position});
                    if (layout(transform)) api.call(rebuildLayout, nullptr, {transform});
                }
            } catch (const std::exception& e) { log(e.what()); }
            release(i->target);
        }
        layoutEdits.clear();
    }
    void placeRow(void* parent, void* rowTF, float height) {
        auto group = layout(parent);
        if (group) {
            if (!autoHeight(parent)) growDown(parent, height + 12);
            api.call(lastSibling, rowTF);
            api.call(rebuildLayout, nullptr, {parent});
        } else {
            auto position = api.value<V3>(localPosition, rowTF);
            auto r = api.value<Rect>(rect, rowTF);
            position.y = bottom(parent, rowTF) - 12 - (r.y + r.height);
            api.call(setLocalPosition, rowTF, {&position});
        }
        void* content = nullptr;
        for (auto current = parent; current; current = api.call(getParent, current)) {
            auto scroll = component(api.call(componentGO, current), "UnityEngine.UI.ScrollRect");
            if (scroll) {
                auto candidate = api.call(getScrollContent, scroll);
                if (candidate && (candidate == parent || api.value<bool>(isChildOf, parent, {candidate}))) { content = candidate; break; }
            }
        }
        // Expand only the existing content hierarchy; no canvas or overlay is made.
        for (auto current = parent; current && isRect(current); current = api.call(getParent, current)) {
            const auto r = api.value<Rect>(rect, current);
            if (!autoHeight(current)) growDown(current, std::max(0.f, r.y - bottom(current) + 12));
            if (current == content || !content) break;
        }
        if (group) api.call(rebuildLayout, nullptr, {parent});
    }
    void createRow(void* sourceItem, void* graphics) {
        auto sourceGO = api.call(componentGO, sourceItem);
        auto sourceTF = api.call(componentTF, sourceItem);
        auto parent = api.call(getParent, sourceTF);
        if (!parent || !isRect(parent)) throw std::runtime_error("Settings row parent missing");
        bool world = false;
        auto clone = api.call(instantiate, nullptr, {sourceGO, parent, &world});
        if (!live(clone)) throw std::runtime_error("Cannot clone native settings row");
        row = weak(clone); // Root cleanup immediately, including partial failure.
        api.call(setName, clone, {api.text(RowName)});
        auto rowRef = component(clone, "UIReference");
        if (!rowRef) throw std::runtime_error("Settings row reference missing");
        auto rowTF = api.call(componentTF, rowRef);
        const auto bounds = api.value<Rect>(rect, rowTF);
        if (!std::isfinite(bounds.height) || bounds.height < 30 || bounds.height > 400)
            throw std::runtime_error("Invalid native settings row template");
        if (auto item = component(clone, "ToggleItem")) {
            bool off = false; api.call(setBehaviourEnabled, item, {&off});
        }
        for (int index : {1, 2, 3, 4, 5, 6}) if (auto go = at(rowRef, index)) {
            bool off = false; api.call(setActive, go, {&off});
        }
        auto titleObject = component(at(rowRef, 0), "UnityEngine.UI.Text");
        if (!titleObject) throw std::runtime_error("Settings row title missing");
        setupText(titleObject, "修改角色模型（原始模型）", true);
        title = weak(titleObject);
        auto frameButton = at(graphics, 0);
        const float buttonHeight = std::clamp(bounds.height - 12, 32.f, 48.f);
        auto prev = makeButton(frameButton, rowTF, "PreviousModel", "上一项", -132, buttonHeight);
        auto following = makeButton(frameButton, rowTF, "NextModel", "下一项", -16, buttonHeight);
        previous = weak(prev); next = weak(following); donor = weak(sourceItem);
        bool on = true; api.call(setActive, clone, {&on});
        placeRow(parent, rowTF, bounds.height);
        previousPointer.store(prev); nextPointer.store(following);
        updateTitle();
        log("Native graphics model selector added");
    }
    void update() {
        const auto now = Clock::now();
        if (now >= nextCatalog) { nextCatalog = now + std::chrono::milliseconds(500); refreshCatalog(); }
        auto menu = ref(owner);
        if (!live(menu)) {
            clearRow(true); release(owner); menu = nullptr;
        }
        if (!menu || !activeComponent(menu)) {
            if (now < nextSearch) return;
            nextSearch = now + std::chrono::milliseconds(500);
            void* found = nullptr;
            for (auto object : api.elements<void*>(api.call(findObjects, nullptr, {api.type("SettingsUI")}), 8))
                if (activeComponent(object)) { found = object; break; }
            if (!found) return;
            if (found != menu) {
                clearRow(true); release(owner); owner = weak(found);
            }
            menu = found;
        }
        if (!activeComponent(menu) || now < nextRefresh) return;
        nextRefresh = now + std::chrono::milliseconds(250);
        auto graphics = read(menu, graphicsField);
        if (!activeComponent(graphics)) return;
        auto source = read(menu, donorField);
        if (!activeComponent(source)) return; // Awake/BindImageEvent still initializing.
        if (source != ref(donor) || !live(ref(row))) {
            clearRow(true);
            try { createRow(source, graphics); }
            catch (...) { clearRow(true); throw; }
        }
        updateTitle();
    }
};
State& state() { static auto* value = new State; return *value; }

void reportError(const std::exception& e) {
    auto& s = state(); const auto now = Clock::now();
    if (now >= s.nextError) { s.nextError = now + std::chrono::seconds(5); log(e.what()); }
}
bool handle(void* button, void* eventData, bool pointer) {
    auto& s = state();
    if (!button || (button != s.previousPointer.load() && button != s.nextPointer.load())) return false;
    bool consumed = true;
    try { consumed = s.click(button, eventData, pointer); }
    catch (const std::exception& e) { reportError(e); }
    s.api.clearTemps();
    return consumed;
}
void pointerClick(void* button, void* eventData, const void* method) {
    if (!handle(button, eventData, true)) {
        ButtonCallback original;
        { std::lock_guard lock(state().installMutex); original = originalPointer; }
        original(button, eventData, method);
    }
}
void submit(void* button, void* eventData, const void* method) {
    if (!handle(button, eventData, false)) {
        ButtonCallback original;
        { std::lock_guard lock(state().installMutex); original = originalSubmit; }
        original(button, eventData, method);
    }
}
}

void initialize(std::string filesDir) {
    auto& s = state(); std::lock_guard lock(s.saveMutex);
    s.path = std::move(filesDir) + "/model-selection.txt";
    std::ifstream file(s.path, std::ios::binary);
    if (!file) return;
    std::string bytes(8192, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(file.gcount()));
    ModelSelection check;
    if (check.decode(bytes)) s.savedBytes = std::move(bytes);
}
void install(const std::vector<void*>& images) {
    auto& s = state(); std::lock_guard lock(s.installMutex);
    if (s.installed.load()) return;
    if (s.nativeAttempted) throw std::runtime_error("A previous settings-hook installation failed");
    try {
        s.bind(images);
        auto pointer = s.api.method("UnityEngine.UI.Button", "OnPointerClick", {"UnityEngine.EventSystems.PointerEventData"}, "System.Void");
        auto keyboard = s.api.method("UnityEngine.UI.Button", "OnSubmit", {"UnityEngine.EventSystems.BaseEventData"}, "System.Void");
        auto a = il2cpp::methodPointer(pointer), b = il2cpp::methodPointer(keyboard);
        if (!a || !b || a == b ||
                !il2cpp::executable(reinterpret_cast<uintptr_t>(a), 16) || !il2cpp::executable(reinterpret_cast<uintptr_t>(b), 16))
            throw std::runtime_error("Native settings button entry points are unavailable");
        s.nativeAttempted = true;
        dobby_dummy_func_t original = nullptr;
        int result = DobbyHook(a, reinterpret_cast<dobby_dummy_func_t>(pointerClick), &original);
        originalPointer = reinterpret_cast<ButtonCallback>(original);
        if (result != 0 || !originalPointer)
            throw std::runtime_error("Cannot hook native Button.OnPointerClick");
        original = nullptr;
        result = DobbyHook(b, reinterpret_cast<dobby_dummy_func_t>(submit), &original);
        originalSubmit = reinterpret_cast<ButtonCallback>(original);
        if (result != 0 || !originalSubmit)
            throw std::runtime_error("Cannot hook native Button.OnSubmit");
        s.installed.store(true);
        log("Native graphics selector hooks ready");
    } catch (const std::exception& e) { log(e.what()); throw; }
}
void tick() {
    auto& s = state(); if (!s.installed.load()) return;
    try { s.update(); }
    catch (const std::exception& e) { reportError(e); }
    s.api.clearTemps();
}
}
