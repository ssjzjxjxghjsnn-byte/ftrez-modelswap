#include "InterceptEntry.h"
#include "Interceptor.h"

InterceptEntry::InterceptEntry(InterceptEntryType type, addr_t address)
    : id(0), type(type), routing(nullptr), patched_addr(0), patched_size(0),
      relocated_addr(0), relocated_size(0), origin_insns{}, origin_insn_size(0), thumb_mode(false) {

#if defined(TARGET_ARCH_ARM)
  if (address % 2) {
    address -= 1;
    this->thumb_mode = true;
  }
#endif

  this->patched_addr = address;
  this->id = Interceptor::SharedInstance()->count();
}
