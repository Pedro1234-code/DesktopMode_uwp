# Guest CRT heap policy

CrtHeapPolicy is owned by CrtProcessState. Its _PNH handler and new-mode belong
to the guest process; installing them never calls the host _set_new_handler or
_set_new_mode. Handler access is synchronized and the lock is released before
calling guest code, allowing reentry, replacement and exceptions.

Resolved for MSVCRT/UCRT (and existing private UCRT aliases):
- _set_new_handler, _query_new_handler, _callnewh;
- _set_new_mode, _query_new_mode;
- malloc/_malloc_base, calloc/_calloc_base;
- realloc/_realloc_base, _recalloc/_recalloc_base;
- _aligned_malloc/_aligned_offset_malloc;
- _aligned_realloc/_aligned_offset_realloc;
- _aligned_recalloc/_aligned_offset_recalloc.

Allocation wrappers precede the native-data export table. Native allocator
ownership remains unchanged: free, _msize and existing native allocation
families remain compatible with these pointers. This is an allocation policy,
not a new independent heap or a guarantee of pointer isolation from the host.

A failed allocation retries only if guest new-mode is enabled and the current
handler returns nonzero. Callback results are normalized to 0/1. Invalid sizes
and multiplication overflow return ENOMEM before allocation or callback.
Zero-sized allocations request at least one byte; realloc(nonnull,0) frees and
returns null. Realloc failure preserves the original pointer. Recalloc delegates
old-block sizing and tail zeroing to the matching native CRT implementation.

Aligned requests validate power-of-two nonzero alignment, offset and total
allocation size including header/gap/padding before calling the native backend.
Retries invoke the guest handler using this release-layout allocation request.
Nonnull realloc/recalloc with zero total size frees before alignment validation.
The native aligned layout is preserved, so existing _aligned_free/_aligned_msize
continue to work. Recalloc tail initialization remains native.

Limits: string-duplication and debug-specific allocation families still use their
existing native adapters and are not all connected to this policy. Native
allocation is attempted first; if the host explicitly enables its own global
new-mode, native allocation may invoke the host new-handler before returning
failure. This pass does not change that global setting or create an independent
guest heap. Exact debug-heap overhead in the callback size is not modeled. C++
std::new_handler has a different ABI and is not aliased to _PNH; this change does
not implement every decorated C++ new/delete export. Invalid mode values dispatch through the guest invalid-parameter handler; if
it returns, the API reports EINVAL. The host handler is not modified globally.

Reference study: local ReactOS sdk/lib/ucrt/heap/new_handler.cpp, new_mode.cpp,
malloc_base.cpp, calloc_base.cpp, realloc_base.cpp and recalloc.cpp. The retry
policy was implemented independently around the existing native allocator.

Verification: Test-CrtHeap.py passed 16 native UCRT reference contracts without
attempting physical memory exhaustion. CrtHeapFixtures.h exercises deterministic
allocation failure/retry, zero-size normalization, callback reentry/throwing,
rejection, overflow and independent policies. It is integrated into BridgeTests
but has NOT been built or executed. No C++/C# build or guest execution was run.

Test-CrtAlignedHeap.py passed 30 additional native contracts for alignments,
offsets, zero size, invalid parameters, overflow, retained bytes and zeroed tail.
CrtHeapAdapterFixtures.h compiles the actual CrtHeapShims.inl against an injected
CrtProcessState for component testing, including invalid-handler dispatch and
failed realloc preservation. CrtHeapFixtures.h also covers aligned request-size
arithmetic. Both remain unbuilt/unrun; native probes do not execute Bridge code.
