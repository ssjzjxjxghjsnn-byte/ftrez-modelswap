#pragma once
#include "self_model/model_options.h"
#include <cstdint>
#include <functional>
#include "sdk/bindings.h"
#include <vector>

namespace ft::self_model {
void setResourcePath(std::string path);
void prepareBindings(sdk::Symbols symbols);
void installMethodHooks(const std::vector<void*>& images,std::function<bool()> mainThread,std::function<void()> afterUpdate);
SelfModelControls uiControls();
bool wanted();
void* profileObject();


void tick(const std::vector<void*>& images,uintptr_t character,bool matchActive,int frame,uintptr_t actor=0);
}
