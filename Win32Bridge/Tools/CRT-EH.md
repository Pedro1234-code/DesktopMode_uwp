# MSVC x64 exception compatibility

`Bridge/CRT/CrtExceptions.cpp` supplies the compiler-coupled exception ABI. It does
not load arbitrary desktop CRTs into the host or emulate a successful catch.
The implementation uses the App CRT linked to the bridge for FH3, object/type
matching, funclets, destruction, and rethrow. FH4 uses the matching App CRT
satellite from the package dependency graph. Desktop native fixtures use their
own linked CRT, including when built with `/MT`.

## Image and thread lifetime

- `RuntimeImage::Reserve` registers the actual allocation and size. Release
  unregisters it after detach and unwind-table removal. This registry remains
  valid even when the DLL loader removes a module from its name lookup during
  unloading.
- `_CxxThrowException` supplies the real image base in the fourth x64 exception
  parameter. A missing host loader entry does not turn a native guest throw into
  a managed/pure throw. WinRT preparation and rethrows retain their ABI paths.
- `RtlPcToFileHeader` queries these guest images and then the native image
  lookup. Ordinary heap/stack allocations are no longer reported as images.
- `GuestModuleScope` selects exception settings owned by the current guest
  process/native thread. New guest threads reset settings when a native thread
  ID is reused. A handler installed by DLL initialization persists into main and
  subsequent callbacks on that guest thread.
- `InvokeCrtExecution` installs a thread-local termination dispatcher and
  restores the host terminate handler and current-exception/context/processing
  slots in `__finally`. Its C++ body uses `/EHsc`; hardware faults continue to
  the existing SEH boundary.
- An unhandled C++ exception, `terminate`, or `purecall` leaves the protected
  callback with a failure result. The termination dispatcher does not raise an
  exception through UCRT's user-handler filter (which would otherwise abort the
  host). Normal caught exceptions still use native unwinding and destructors.

The protected entry paths include EXE and thread entry, TLS, DllMain, window
procedures, CallWindowProc/subclass procedures, hooks, async send callbacks,
dialog/task-dialog callbacks, and FLS callbacks.

## Validation and remaining limits

`Tools/Test-CrtExceptions.py` probes native ownership, type-info comparison/hash,
name caching, thread-local EH slots, terminate handler restoration, and image
lookup. These probes do **not** execute the bridge implementation.

`GuestApps/BridgeTests/CrtExceptionFixtures.h` adds bridge C++ fixtures for
image registration, typed/base-class catches, destructor/unwind actions,
rethrow, nested boundaries, noexcept termination, a returning/failing user
terminate handler, active host catch preservation, and per-guest settings.
These require the user-run C++ build and execution; they have not been run by
the agent.

FH4 requires `vcruntime140_1_app.dll` (or `vcruntime140_1d_app.dll` for the bridge Debug CRT) in the host package dependency graph. A
missing satellite is logged as such and remains an unresolved import. It does
not introduce a mandatory DLL import that prevents the whole bridge from
loading, and FH3 is never substituted for incompatible FH4 metadata. The bridge EH source itself is compiled with `/d2FH4-` so its own guard does not add a static FH4 dependency.

This is the MSVC ABI, not an implementation of the GNU C++ ABI. Full MSVCP/STL DLL coverage, legacy RTTI locator revision 0 on manually mapped
images, and execution validation on Xbox remain separate compatibility work. Statically linked guest runtimes
continue to execute their own implementations through the available NT/Win32
adapters.

## References

- Local ReactOS: `dll/win32/msvcrt/except.c`, `handler4.c`, `cpp.c`.
- Installed MSVC source: `vcruntime/throw.cpp`, `risctrnsctrl.cpp`,
  `ehhelpers.cpp`, `std_exception.cpp`, and `std_type_info.cpp`.
- [Microsoft: __CxxFrameHandler3](https://learn.microsoft.com/en-us/windows/win32/memory/cxxframehandler3)
- [Microsoft: set_terminate](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/set-terminate-crt?view=msvc-170)
- [Microsoft: FH4 metadata/runtime](https://devblogs.microsoft.com/cppblog/making-cpp-exception-handling-smaller-x64/)

## Exception pointers, handlers and unwind hardening

- Virtual msvcp140.dll and msvcp140_app.dll expose the 10 explicit x64
  exception_ptr helpers, including their decorated ABI names. Other STL exports
  can resolve from an application-provided guest DLL image. Static import
  binding and dynamic loads prefer that guest image. The virtual module
  exposes the exception_ptr helpers as a fallback when the guest DLL is
  absent.
- CopyException handles manually mapped ThrowInfo by presenting a temporary
  native x64 exception record with the registered throw image base to the native
  current_exception copier. The current-exception slot is restored in __finally.
  Native shared ownership, copy constructors, destructors and rethrow remain in
  the matching linked MSVC runtime. Guest images must remain mapped while their
  code/metadata is referenced, as with native DLL exception objects.
- Invalid-parameter handlers have per-guest process and per-thread settings;
  thread-local handlers take precedence. Boundary entry installs a dispatcher
  in the native UCRT thread slot and restores the host slot on exit. Bridge
  _invoke_watson and noreturn validation paths terminate the guest boundary.
  This does not intercept every internal fail-fast path of native CRT code.
- Purecall configuration belongs to the guest process. A returning handler is
  followed by guest termination rather than a fake successful virtual call.
- SEH translators are retained per guest thread, installed in native thread EH
  state at boundary entry and restored on exit. Actual hardware-to-C++
  translation follows the guest frame's /EHa metadata and native frame handler;
  /EHsc is not silently changed. End-to-end translated-fault testing is pending.
- __RTDynamicCast, __RTtypeid, __RTCastToVoid and _is_exception_typeof route to
  the matching MSVC runtime. Modern x64 locator revision 1 uses its self RVA;
  it does not need host module enumeration. Revision 0 retains the native
  loader-dependent limitation and has not been separately ported.
- RuntimeImage checks structural .pdata/.xdata bounds/readability, supported
  header versions, handler RVAs and chained/indirect entries before native
  registration. Cycles, conflicting handler/chain flags and duplicate starts
  are rejected. An all-zero padded table needs no registration. This is not a
  full verifier for every UWOP instruction, FH3/FH4 language payload or PC range.

Additional native reference probes passed: 13 exception_ptr/invalid-handler
contracts and 3 RTTI contracts. The exception_ptr probe verifies deep copy using
an explicit image-relative throw record without registering the image with the
host loader. The RTTI probe similarly uses a self-relative locator outside host
modules. Together with Test-CrtExceptions.py (15) and Test-CrtHeap.py (16), this
pass has 47 passing native reference contracts; these do not execute bridge C++.

New fixtures: CrtExceptionPtrFixtures.h, CrtExceptionPolicyFixtures.h,
CrtRttiFixtures.h and X64UnwindFixtures.h. They cover retained exception objects,
empty exception_ptr rethrow, cross-thread rethrow, typed/base catches,
invalid-handler precedence, purecall, translator restoration, RTTI cross-casts,
bad_cast/bad_typeid and malformed unwind chains. All are integrated, unbuilt
and unrun. No claim of production-ready CRT or proven exception compatibility
can be made until these and real manually mapped guest PEs pass user builds and
runtime tests. Full STL, complete native-allocator failure isolation, MBCS virtualization and
remaining stdio/descriptor semantics are still work. Guest process/thread locale
selection is now implemented; see CRT-LOCALE.md for coverage and limits. The aligned
allocation family is now connected to the guest policy (see CRT-HEAP.md).

Boundary isolation now clears current-exception/context/processing slots on an
outer guest entry or a switch to a different guest thread state. Nested entry
into the same guest retains its active catch. The saved host/outer guest slots
are restored in __finally. Isolation fixtures check that a host catch cannot be
captured by guest current_exception(), that the host can still rethrow, and
that nested guest callbacks can capture the active guest exception. These
fixtures remain unbuilt/unrun.

## CRT error and random state per guest thread

The selected CrtThreadExceptionState now retains errno, _doserrno and the rand
seed as well as EH settings. On outer/cross-guest entry, InvokeCrtExecution
selects the guest error values in the native CRT TLS cells. On return, hardware
fault unwinding or controlled guest termination, __finally commits guest values
and restores the host/suspended guest cells. Same-guest nested callbacks share
the active values. A -> B -> A reentry restores A's latest error state instead
of B's stale snapshot. Internal handler setup/teardown does not replace the
caller's error result. Native _errno/__doserrno pointers still refer to native
TLS while selected; this is context isolation, not separate address spaces or
independent pointers for simultaneously suspended logical guests.

rand/srand use the retained guest thread seed, initially 1. Reused native worker
threads do not leak a prior guest seed. Resetting the thread record for a fresh
guest thread also resets errors and seed. The host's native srand is not called.
__doserrno/_get_doserrno/_set_doserrno now have explicit native data routes,
matching the selected native error cells and existing _errno/get/set routes.

Test-CrtThreadState.py passed 10 native reference contracts for TLS error cells,
getter/setter semantics, null-output validation, independent thread values and
known random sequences. CrtThreadStateFixtures.h covers actual boundary error
selection, nested callbacks, cross-guest reentry, termination, host restoration,
seed persistence and record reset. Fixtures remain unbuilt/unrun. Locale now has a guest process-wide shared/private model with explicit native
_locale_t dispatch, without switching caller native locale. The protected
boundary retains snapshots through reentrant callbacks and releases references
and storage explicitly in __finally, including longjmp/SEH. See CRT-LOCALE.md.
Independent MBCS policy and memory-only adapters are now implemented (see
CRT-MULTIBYTE.md). Raw MBCS globals and full MSVCP
locale/facet ABI coverage remain separate work.
