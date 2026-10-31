#pragma once
#include <string>
#include <vector>

namespace ft::settings {
// initialize performs file I/O only. install resolves metadata/hooks on an
// attached worker. tick runs once per frame on Unity's main thread before rendering.
void initialize(std::string filesDir);
void install(const std::vector<void*>& images);
void tick();
}
