#pragma once
#include <array>
#include <string>
#include <vector>
#include "sdk/bindings.h"

namespace ft::self_model {
struct LobbyRenderTarget {
    void* host=nullptr;
    std::vector<void*> renderers;
};
// All replacement instances (Self, Rabbit, own Profile), after model handoffs.
// The lobby source is separate so Rabbit can capture it with Self disabled.
void updateLobbyRender(const std::vector<void*>& images, const sdk::Symbols& symbols, bool inLobby,
                       const std::array<LobbyRenderTarget,3>& targets, bool enabled);
std::string lobbyRenderStatus();
// Uses the same metadata-bound, active lobby UI/camera as environment capture.
bool isLobbyModelHost(const std::vector<void*>& images, const sdk::Symbols& symbols, void* host);
}
