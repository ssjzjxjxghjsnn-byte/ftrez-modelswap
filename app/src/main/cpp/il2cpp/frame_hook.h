#pragma once
#include <functional>
#include <vector>

namespace ft::il2cpp::frame_hook {
// Install on an attached IL2CPP worker. The callback runs before Unity starts
// the SRP render loop or invokes built-in camera pre-cull listeners. The caller
// owns main-thread filtering and frame-count deduplication across cameras.
// Throws if metadata or either native hook is unavailable. A failed native
// installation stays disabled; an address is never patched a second time.
void install(const std::vector<void*>& images, std::function<void()> beforeRender);
}
