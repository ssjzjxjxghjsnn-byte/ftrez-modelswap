#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace ft::sdk {
struct OwnPlayerState {
    uintptr_t character=0,actor=0;
    bool matchActive=false;
};
class OwnPlayer {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    OwnPlayer();
    ~OwnPlayer();
    void bind(const std::vector<void*>& images);
    // Unity main thread only. Returned objects remain rooted until the next update.
    OwnPlayerState update();
};
}
