#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/native-tests
dobby_src=app/src/main/cpp/third_party/Dobby
common=(
  -std=c++17 -DDISABLE_ARCH_DETECT -DTARGET_ARCH_ARM64 -D__ANDROID__ -DANDROID
  -Itests/native/dobby_stubs
  "-I$dobby_src" "-I$dobby_src/include" "-I$dobby_src/source" "-I$dobby_src/source/include"
  "-I$dobby_src/external" "-I$dobby_src/external/logging" "-I$dobby_src/source/Backend/UserMode"
  tests/native/dobby_failure_test.cpp
  "$dobby_src/source/InterceptEntry.cpp" "$dobby_src/source/Interceptor.cpp"
  "$dobby_src/source/InterceptRouting/InterceptRouting.cpp"
  "$dobby_src/source/InterceptRouting/Routing/FunctionInlineHook/FunctionInlineHook.cc"
  "$dobby_src/source/InterceptRouting/Routing/FunctionInlineHook/RoutingImpl.cc"
  "$dobby_src/source/InterceptRouting/RoutingPlugin/RoutingPlugin.cc"
  "$dobby_src/source/MemoryAllocator/CodeBuffer/CodeBufferBase.cc"
  "$dobby_src/source/MemoryAllocator/MemoryAllocator.cc"
  "$dobby_src/source/MemoryAllocator/AssemblyCodeBuilder.cc"
  "$dobby_src/source/core/assembler/assembler.cc"
  "$dobby_src/source/core/assembler/assembler-arm64.cc"
  "$dobby_src/source/core/codegen/codegen-arm64.cc"
  "$dobby_src/source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc"
)
"${CXX:-g++}" "${common[@]}" \
  "$dobby_src/source/InstructionRelocation/arm64/InstructionRelocationARM64.cc" \
  "$dobby_src/source/TrampolineBridge/Trampoline/arm64/trampoline_arm64.cc" \
  -o build/native-tests/dobby_failure_test.exe
for scenario in success null_address null_replacement entry_permission_failure allocation_failure \
    arena_permission_failure relocation_write_failure relocation_restore_failure \
    commit_write_failure commit_restore_failure patch_ranges unprepared_commit; do
  build/native-tests/dobby_failure_test.exe "$scenario"
done
"${CXX:-g++}" "${common[@]}" -DDOBBY_TEST_FAKE_CODEGEN -o build/native-tests/dobby_invalid_codegen_test.exe
for scenario in null_trampoline empty_trampoline oversized_trampoline empty_relocation \
    null_relocation_address zero_relocation_size oversized_prologue short_prologue moved_prologue; do
  build/native-tests/dobby_invalid_codegen_test.exe "$scenario"
done
