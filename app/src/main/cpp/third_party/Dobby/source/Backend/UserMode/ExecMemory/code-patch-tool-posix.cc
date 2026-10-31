
#include "dobby_internal.h"
#include "core/arch/Cpu.h"

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

#if !defined(__APPLE__)
PUBLIC MemoryOperationError DobbyCodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
#if defined(__ANDROID__) || defined(__linux__)
  const long page_size = sysconf(_SC_PAGESIZE);
  const uintptr_t start = (uintptr_t)address;
  if (!address || !buffer || !buffer_size || page_size <= 0 || start > UINTPTR_MAX - buffer_size)
    return kMemoryOperationError;
  const uintptr_t patch_page = start - start % page_size;
  const uintptr_t last_byte = start + buffer_size - 1;
  const uintptr_t end_page = last_byte - last_byte % page_size;
  if (end_page > UINTPTR_MAX - (uintptr_t)page_size)
    return kMemoryOperationError;
  const size_t span = end_page - patch_page + page_size;

  // Check the whole range before copying any instruction bytes.
  if (mprotect((void *)patch_page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    return kMemoryOperationError;

  // patch buffer
  memcpy(address, buffer, buffer_size);

  // restore page permission
  const int restore_result = mprotect((void *)patch_page, span, PROT_READ | PROT_EXEC);

  addr_t clear_start_ = (addr_t)address;
  ClearCache((void *)clear_start_, (void *)(clear_start_ + buffer_size));
  // The patch can already be live on this failure path. Do not undo/repatch it.
  return restore_result == 0 ? kMemoryOperationSuccess : kMemoryOperationError;
#else
  return kMemoryOperationError;
#endif
}

#endif
