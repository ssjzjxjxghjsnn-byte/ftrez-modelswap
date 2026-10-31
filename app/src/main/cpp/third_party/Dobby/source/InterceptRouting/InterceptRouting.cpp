#include "dobby_internal.h"

#include "InterceptRouting/InterceptRouting.h"
#include "InterceptRouting/RoutingPlugin/RoutingPlugin.h"

using namespace zz;

void log_hex_format(uint8_t *buffer, uint32_t buffer_size) {
  char output[1024] = {0};
  for (int i = 0; i < buffer_size && i < sizeof(output); i++) {
    snprintf(output + strlen(output), 3, "%02x ", *((uint8_t *)buffer + i));
  }
  DLOG(0, "%s", output);
};

void InterceptRouting::Prepare() {
}

// generate relocated code
bool InterceptRouting::GenerateRelocatedCode() {
  entry_->relocated_addr = 0;
  entry_->relocated_size = 0;
  entry_->origin_insn_size = 0;
  auto trampoline = GetTrampolineBuffer();
  if (!trampoline || !trampoline->GetBuffer() || !trampoline->GetBufferSize() ||
      trampoline->GetBufferSize() > sizeof(entry_->origin_insns)) {
    return false;
  }
  uint32_t tramp_size = trampoline->GetBufferSize();
  origin_ = new CodeMemBlock(entry_->patched_addr, tramp_size);
  relocated_ = new CodeMemBlock();

  auto buffer = (void *)entry_->patched_addr;
#if defined(TARGET_ARCH_ARM)
  if (entry_->thumb_mode) {
    buffer = (void *)((addr_t)buffer + 1);
  }
#endif
  GenRelocateCodeAndBranch(buffer, origin_, relocated_);
  if (!relocated_->addr || !relocated_->size || relocated_->size > UINT32_MAX ||
      origin_->addr != entry_->patched_addr || origin_->size < tramp_size ||
      origin_->size > sizeof(entry_->origin_insns)) {
    ERROR_LOG("[insn relocate]] failed");
    return false;
  }

  // save original prologue
  memcpy((void *)entry_->origin_insns, (void *)origin_->addr, origin_->size);
  entry_->origin_insn_size = origin_->size;

  // Publish only after the entire relocation and saved prologue are valid.
  entry_->relocated_addr = relocated_->addr;
  entry_->relocated_size = relocated_->size;

  // log
  DLOG(0, "[insn relocate] origin %p - %d", origin_->addr, origin_->size);
  log_hex_format((uint8_t *)origin_->addr, origin_->size);

  DLOG(0, "[insn relocate] relocated %p - %d", relocated_->addr, relocated_->size);
  log_hex_format((uint8_t *)relocated_->addr, relocated_->size);

  return true;
}

bool InterceptRouting::GenerateTrampolineBuffer(addr_t src, addr_t dst) {
  // if near branch trampoline plugin enabled
  if (RoutingPluginManager::near_branch_trampoline) {
    auto plugin = static_cast<RoutingPluginInterface *>(RoutingPluginManager::near_branch_trampoline);
    if (plugin->GenerateTrampolineBuffer(this, src, dst) == false) {
      DLOG(0, "Failed enable near branch trampoline plugin");
    }
  }

  if (GetTrampolineBuffer() == nullptr) {
    auto tramp_buffer = GenerateNormalTrampolineBuffer(src, dst);
    SetTrampolineBuffer(tramp_buffer);
  }
  auto buffer = GetTrampolineBuffer();
  return buffer && buffer->GetBuffer() && buffer->GetBufferSize() &&
         buffer->GetBufferSize() <= sizeof(entry_->origin_insns);
}

// active routing, patch origin instructions as trampoline
bool InterceptRouting::IsReadyToCommit() {
  return entry_->relocated_addr && entry_->relocated_size && entry_->origin_insn_size &&
         trampoline_buffer_ && trampoline_buffer_->GetBuffer() && trampoline_buffer_->GetBufferSize() &&
         trampoline_buffer_->GetBufferSize() <= entry_->origin_insn_size &&
         entry_->origin_insn_size <= sizeof(entry_->origin_insns);
}

bool InterceptRouting::Active() {
  if (!IsReadyToCommit()) {
    ERROR_LOG("[intercept routing] refusing to patch without a valid relocation");
    return false;
  }
  MemoryOperationError err;
  err = DobbyCodePatch((void *)entry_->patched_addr, trampoline_buffer_->GetBuffer(),
                       trampoline_buffer_->GetBufferSize());
  if (err != kMemoryOperationSuccess) {
    ERROR_LOG("[intercept routing] active failed");
    return false;
  }
  DLOG(0, "[intercept routing] active");
  return true;
}

bool InterceptRouting::Commit() {
  return this->Active();
}

#if 0
int InterceptRouting::PredefinedTrampolineSize() {
#if __arm64__
  return 12;
#elif __arm__
  return 8;
#endif
}
#endif

InterceptEntry *InterceptRouting::GetInterceptEntry() {
  return entry_;
};
