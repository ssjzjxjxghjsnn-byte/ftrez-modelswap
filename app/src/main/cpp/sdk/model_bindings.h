#pragma once
#include "sdk/bindings.h"
namespace ft::sdk {
void loadConfig(const std::string& file);
const std::string& assemblyName();
const Symbols& modelBindings();
const Symbols& ownPlayerBindings();
const Symbols& settingsBindings();

// Resolve exactly the configured assembly, declaring type and member signature.
void* resolveType(const std::vector<void*>& images,const Symbol& symbol);
void* resolveMethod(const std::vector<void*>& images,const Symbol& symbol);
// Includes declaring owner, flags, type and reflected MetadataToken checks.
void* resolveField(const std::vector<void*>& images,const Symbol& symbol);
// Reject mismatched images, metadata rows, storage flags and member signatures.
void validateBindings(const std::vector<void*>& images,const Symbols& symbols);
}
