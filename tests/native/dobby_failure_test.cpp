// Compiles the bundled DobbyHook, routing, ARM64 relocation/finalization,
// allocator and POSIX patch implementation. Only OS services are simulated.
// ARM64 bytes are inspected on this x64 host, not executed.
#include "dobby_internal.h"
#include "Interceptor.h"
#include "InterceptRouting/Routing/FunctionInlineHook/FunctionInlineHookRouting.h"
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr size_t pageSize = 4096;
alignas(pageSize) std::array<uint8_t, pageSize * 4> entryBytes{};
alignas(pageSize) std::array<uint8_t, pageSize * 4> execBytes{};
std::string scenario;
int allocations = 0, frees = 0, entryWrites = 0, execWrites = 0, cacheClears = 0;
long reportedPageSize = pageSize;
bool standalonePatch = false;
struct ProtectionCall { void* address; size_t size; int protection; };
std::vector<ProtectionCall> protectionCalls;
dobby_dummy_func_t* publishedOriginal = nullptr;

void require(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void replacement() {}
bool isEntry(void* ptr) {
  const auto p = reinterpret_cast<uintptr_t>(ptr);
  return p >= reinterpret_cast<uintptr_t>(entryBytes.data()) &&
         p < reinterpret_cast<uintptr_t>(entryBytes.data() + entryBytes.size());
}
void checkOriginal(dobby_dummy_func_t original) {
  require(original == reinterpret_cast<dobby_dummy_func_t>(execBytes.data()),
          "Original must point to the successfully emitted executable block");
  const uint32_t nop = 0xd503201f;
  require(!std::memcmp(execBytes.data(), &nop, sizeof(nop)), "Original must contain relocated ARM64 instructions");
}
}

extern "C" int log_internal_impl(int, const char*, ...) { return 0; }
int OSMemory::PageSize() { return static_cast<int>(pageSize); }
void* OSMemory::Allocate(size_t size, MemoryPermission) {
  ++allocations;
  require(size <= execBytes.size(), "Test allocation must fit its backing storage");
  return scenario == "allocation_failure" ? nullptr : execBytes.data();
}
bool OSMemory::SetPermission(void* address, size_t, MemoryPermission) {
  if (isEntry(address)) return scenario != "entry_permission_failure";
  return scenario != "arena_permission_failure";
}
bool OSMemory::Free(void* address, size_t) {
  require(address == execBytes.data(), "Only failed, unreachable arena allocation may be released");
  ++frees; return true;
}
long dobby_test_sysconf(int) { return reportedPageSize; }
int dobby_test_mprotect(void* address, size_t size, int protection) {
  protectionCalls.push_back({address, size, protection});
  const bool entry = isEntry(address), writing = (protection & PROT_WRITE) != 0;
  if (writing) {
    if (entry) ++entryWrites; else ++execWrites;
    if (entry && !standalonePatch) {
      require(publishedOriginal && *publishedOriginal,
              "Actual DobbyHook must publish a nonnull original before any entry write attempt");
      require(Interceptor::SharedInstance()->find(reinterpret_cast<addr_t>(entryBytes.data())) != nullptr,
              "Commit attempt must already be reserved against a second hook");
      checkOriginal(*publishedOriginal);
    }
  }
  if (entry && scenario == "commit_write_failure" && writing) return -1;
  if (entry && scenario == "commit_restore_failure" && !writing) return -1;
  if (!entry && scenario == "relocation_write_failure" && writing) return -1;
  if (!entry && scenario == "relocation_restore_failure" && !writing) return -1;
  return 0;
}
void ClearCache(void* begin, void* end) {
  require(reinterpret_cast<uintptr_t>(end) > reinterpret_cast<uintptr_t>(begin), "Cache range must be nonempty");
  ++cacheClears;
}

#if defined(DOBBY_TEST_FAKE_CODEGEN)
// Malformed code-generator results exercise the real entry/routing guards.
// The default executable below instead uses the real ARM64 generator/relocator.
CodeBufferBase* GenerateNormalTrampolineBuffer(addr_t, addr_t) {
  if (scenario == "null_trampoline") return nullptr;
  auto buffer = new CodeBufferBase();
  if (scenario == "empty_trampoline") return buffer;
  const size_t size = scenario == "oversized_trampoline" ? 257 : 16;
  for (size_t i = 0; i < size; ++i) buffer->Emit8(0x42);
  return buffer;
}
void GenRelocateCodeAndBranch(void*, CodeMemBlock* origin, CodeMemBlock* relocated) {
  if (scenario == "empty_relocation") return;
  if (scenario == "null_relocation_address") { relocated->reset(0, 16); return; }
  if (scenario == "zero_relocation_size") { relocated->reset(reinterpret_cast<addr_t>(execBytes.data()), 0); return; }
  relocated->reset(reinterpret_cast<addr_t>(execBytes.data()), 16);
  if (scenario == "oversized_prologue") origin->size = 257;
  else if (scenario == "short_prologue") origin->size = 4;
  else if (scenario == "moved_prologue") origin->addr += 4;
  else require(false, "Unknown fake-codegen scenario");
}
#endif

int main(int argc, char** argv) {
  require(argc == 2, "Provide one failure scenario");
  scenario = argv[1];
  for (size_t i = 0; i < entryBytes.size(); i += 4) {
    const uint32_t nop = 0xd503201f;
    std::memcpy(entryBytes.data() + i, &nop, sizeof(nop));
  }
  const auto originalBytes = entryBytes;
  if (scenario == "patch_ranges") {
    standalonePatch = true;
    std::array<uint8_t, pageSize * 3> patch{};
    patch.fill(0x7a);
    require(DobbyCodePatch(entryBytes.data(), patch.data(), pageSize) == kMemoryOperationSuccess, "Page-sized patch must succeed");
    require(protectionCalls.size() == 2 && protectionCalls[0].size == pageSize && protectionCalls[1].size == pageSize,
            "Exact page boundary must not touch the next page");
    protectionCalls.clear();
    require(DobbyCodePatch(entryBytes.data() + 7, patch.data(), patch.size()) == kMemoryOperationSuccess, "Multi-page patch must succeed");
    require(protectionCalls.size() == 2 && protectionCalls[0].address == entryBytes.data() &&
            protectionCalls[0].size == pageSize * 4 && protectionCalls[1].size == pageSize * 4,
            "Permission changes must include every covered page");
    require(!std::memcmp(entryBytes.data() + 7, patch.data(), patch.size()), "Patch bytes must be copied exactly");
    protectionCalls.clear();
    require(DobbyCodePatch(nullptr, patch.data(), 1) != kMemoryOperationSuccess, "Null destination must fail");
    require(DobbyCodePatch(entryBytes.data(), nullptr, 1) != kMemoryOperationSuccess, "Null buffer must fail");
    require(DobbyCodePatch(entryBytes.data(), patch.data(), 0) != kMemoryOperationSuccess, "Empty patch must fail");
    require(DobbyCodePatch(reinterpret_cast<void*>(UINTPTR_MAX - 3), patch.data(), 8) != kMemoryOperationSuccess, "Overflowing range must fail");
    require(DobbyCodePatch(reinterpret_cast<void*>(UINTPTR_MAX - 3), patch.data(), 1) != kMemoryOperationSuccess, "Overflowing aligned range must fail");
    reportedPageSize = -1;
    require(DobbyCodePatch(entryBytes.data(), patch.data(), 1) != kMemoryOperationSuccess, "sysconf failure must fail");
    require(protectionCalls.empty(), "Invalid inputs must not reach mprotect or memcpy");
  } else if (scenario == "unprepared_commit") {
    InterceptEntry entry(kFunctionInlineHook, reinterpret_cast<addr_t>(entryBytes.data()));
    require(!entry.relocated_addr && !entry.relocated_size && !entry.origin_insn_size && !entry.thumb_mode,
            "Failed relocation must start from initialized state");
    FunctionInlineHookRouting routing(&entry, replacement);
    require(!routing.Commit() && protectionCalls.empty(), "Unprepared routing must refuse to patch");
  } else {
    dobby_dummy_func_t original = replacement; // Must be cleared on precommit errors.
    publishedOriginal = &original;
    void* address = scenario == "null_address" ? nullptr : entryBytes.data();
    auto replace = scenario == "null_replacement" ? nullptr : replacement;
    const int result = DobbyHook(address, replace, &original);
    const bool commitAttempt = scenario == "success" || scenario == "commit_write_failure" || scenario == "commit_restore_failure";
    require((result == RS_SUCCESS) == (scenario == "success"), "Real DobbyHook must report detectable failures");
    if (commitAttempt) {
      require(entryWrites == 1, "There must be exactly one entry write attempt");
      checkOriginal(original);
      require(Interceptor::SharedInstance()->count() == 1, "Commit attempts must retain their entry/trampoline");
      require((entryBytes != originalBytes) == (scenario != "commit_write_failure"), "Prewrite errors must leave instruction bytes unchanged");
      require(cacheClears == (scenario == "commit_write_failure" ? 1 : 2), "Written bytes must flush cache even on restore failure");
      const auto savedOriginal = original;
      const auto callsBefore = protectionCalls.size();
      dobby_dummy_func_t retry = replacement;
      require(DobbyHook(entryBytes.data(), replacement, &retry) == RS_FAILED && retry == nullptr,
              "Duplicate/uncertain commit may not claim successful installation");
      require(entryWrites == 1 && protectionCalls.size() == callsBefore && original == savedOriginal,
              "Retry must neither patch again nor discard the caller's retained trampoline");
    } else {
      require(original == nullptr && entryWrites == 0 && entryBytes == originalBytes,
              "No failed generation may publish an original or patch the entry");
      require(Interceptor::SharedInstance()->count() == 0, "Failures before commit must not register as hooks");
      if (scenario == "arena_permission_failure") require(frees == 1, "Failed arena permission must release the unreachable allocation");
      if (scenario == "relocation_restore_failure") require(cacheClears == 1, "Failed relocation write restoration must still clear cache");
      if (scenario == "allocation_failure") require(allocations == 1, "Allocation failure must return without dereferencing null");
    }
  }
  std::cout << "dobby_failure_test: " << scenario << " passed\n";
}
