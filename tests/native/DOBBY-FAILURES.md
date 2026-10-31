# Dobby installation failure contract (Android ARM64)

`DobbyHook` now clears the output pointer first and refuses to commit unless the
detour buffer, saved prologue and relocated executable block are valid. ARM64
relocation handles a null finalizer result; the finalizer rejects allocation or
code-write failures. `Active`/`Commit` return the actual code-patch result.

| Failure stage | Original entry bytes | Returned original | Registration |
| --- | --- | --- | --- |
| Invalid argument, trampoline/relocation/finalization failure | Unchanged | Null | None |
| Entry writable-permission failure | Unchanged | Valid | Retained; retry rejected |
| Entry RX-restoration failure | Already written, cache flushed | Valid | Retained; retry rejected |
| Successful commit | Written, cache flushed | Valid | Retained; retry rejected |

The last three rows assume the caller requests an output original. An error does
not mean a detour is absent. Callers must retain a nonnull original after an error
and must not blindly undo or retry the patch. Frame hooks enforce this contract
with their installation mutex and one-attempt state; callbacks stay disabled if
either hook fails, while installed detours keep forwarding every argument.

POSIX patching checks `mprotect` over the entire aligned range before `memcpy`.
It checks restoration separately and flushes the instruction cache even when
restoration fails. No rollback write is attempted. A failed `mprotect` can still
have changed permissions on part of a range; no instruction bytes are copied
when the writable-permission request fails.

Run from the project root in Git Bash with MinGW `g++` and MSVC installed:

```sh
bash tests/native/run_dobby_failure_test.sh
MSYS_NO_PATHCONV=1 cmd.exe /d /c 'tests\native\run_frame_hook_test.bat'
```

The first script runs 12 scenarios against the real bundled DobbyHook, routing,
ARM64 assembler/relocator, finalizer, allocator and POSIX code patcher, faking only
OS allocation/protection/cache services. It verifies output publication and
registration before the source-entry write, permission errors before and after
the copy, allocation failure, unchanged bytes before commit, retained original
after commit errors, duplicate rejection and full-page range handling. A second
build injects nine invalid code-generator outputs into the real Dobby entry and
routing guards, including null-address/nonzero-size relocation. The frame test
runs seven scenarios with independent patch visibility and original publication.

These host tests inspect emitted ARM64 bytes; they do not execute ARM64 code.
They do not establish atomic instruction replacement while other threads execute
the same entry, validate all ARM64 instruction relocation semantics, or recover
from invalid/unmapped source pointers, asynchronous mapping changes, signals or
C++ heap exhaustion. Dobby's global registry still requires serialized installer
calls. This patch hardens the Android ARM64 `DobbyHook` path; it does not claim a
complete failure-contract audit of `DobbyInstrument` or other architectures.
