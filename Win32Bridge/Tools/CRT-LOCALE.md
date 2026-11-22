# Guest CRT locale contexts

CrtLocalePolicy stores an immutable canonical LC_ALL name per guest process.
It starts at C independently of the host locale. An independent immutable MBCS
code-page token starts at the guest ANSI default. Each guest thread has its own
shared/private mode, optional private name, observed state, native snapshot,
return-string storage and localeconv lease. Shared category setters compare
snapshot identity and retry after a peer update, preserving concurrently
changed categories. No policy lock is held across native calls or callbacks.

## Explicit snapshots instead of caller-thread mutation

Protected guest execution no longer selects/restores a native thread locale.
Conversions, ctype, casing and case-insensitive comparison use the native _l
APIs with an explicit guest handle. Buffer printf/scanf receive that same handle.
The existing 53 data adapters now also expose their 53 corresponding _l exports;
a null _locale_t means the guest default, while an explicit handle retains its
native create/free ownership contract. iswctype and the unsafe wide-case
adapters use explicit handles as well.

Native setlocale/_wsetlocale calls needed to canonicalize names, combine
categories and obtain localeconv data execute on a short-lived private helper
thread. Inputs are copied on the guest execution thread before starting the
helper. It never invokes guest callbacks or dereferences guest input pointers.
It explicitly enables native private mode, applies the base canonical name and
applies the independently selected guest MBCS code page. The helper copies all category names and
retains _get_current_locale before exiting. Its errno is reported on failure;
success preserves the executing guest's errno/doserrno.

The execution thread never calls native locale setters, even to query names.
This matters because a native setlocale query can itself recreate PTD locale
metadata. Host private non-C locale identities and borrowed names/lconv data no
longer need to survive a temporary guest locale switch. The host global locale
and native global mode/startup flags are not changed by guest locale policy.

## Ownership, concurrency and exception boundaries

A thread caches its native snapshot until either the immutable locale name or
the independent MBCS token changes.
Repeated conversions do not create helper threads. First use, a new observed
shared snapshot, a category setter or an owned _get_current_locale clone may
create a helper; this is a correctness-first implementation, not a pooled
locale service. Repeated changes/large thread counts can still be optimized.

The protected boundary retains each snapshot used during its callback. Native
validation can reenter the guest and change locale without freeing the handle
still in use by the suspended operation. The leases live outside the noinline
setjmp leaf. Its __finally explicitly releases both vector elements and allocated
capacity, including controlled termination and native SEH paths which bypass
ordinary /EHsc caller destructors. No native locale restoration is needed on
A -> B -> A reentry; each operation resolves its guest's latest policy.

Default mode is shared (_DISABLE_PER_THREAD_LOCALE). Enabling private mode
captures the thread's last observed data; disabling it allows the next operation
to synchronize with shared state. Entering a protected callback does not force
synchronization. The loader initializes the first guest CRT thread from shared
state; new threads start with the process LC categories and initial SBCS MBCS
data, as native PTD initialization does. A fresh thread enabling private mode
before its first MBCS operation therefore does not inherit a recently changed
global MBCS page. Reading shared state first and then enabling private mode
captures that observed page. Startup mode -1 suppresses synchronization for that guest and preserves
the thread's observed state. New threads start from retained process shared
state. These changes never forward native _configthreadlocale to the caller.

setlocale/_wsetlocale copy results into guest-thread storage. Allocation of
return storage precedes publication, so failure cannot silently commit a new
locale. Unsupported names preserve policy. Invalid categories/modes invoke
the guest validation handler before returning EINVAL if it returns. localeconv
retains the snapshot owning its pointed-to data. _get_current_locale returns a
new native-owned handle with the guest's categories and independent MBCS state;
_free_locale is the ordinary matching native release operation.

## Stream scanning

The custom file/stream scanf parser now accepts explicit handles, as buffer
scanf/printf already do. It carries the selected locale through whitespace,
Unicode digit recognition, numeric conversion, narrow/wide character conversion
and lead-byte checks. A memory-only native numeric format supplies the first
radix byte used by UCRT strtox; no private lconv layout is read. Default stream
scanning resolves the guest snapshot once per call. Existing limitations of the
custom scanner (including complete multibyte/UTF-8 edge cases) are not claimed
resolved by adding explicit locale dispatch.

## Remaining scope

- _setmbcp/_getmbcp and 78 pairs of MBCS function adapters are now implemented
  with independent shared/private state; see CRT-MULTIBYTE.md. Raw writable
  _mbctype/_mbcasemap globals and their pointer exports remain separate work.
  LC_CTYPE lead-byte checks in scanning still follow the LC_CTYPE component,
  not an unrelated host table. The restartable wchar_t conversion family uses
  the guest LC_CTYPE snapshot. C11 mbrtoc16/mbrtoc32 and c16rtomb/c32rtomb
  follow UCRT's UTF-8 behavior independently of LC_CTYPE, with guest-owned
  implicit state. UTF-8 edge cases in the custom stream scanner are separate.
- This is not complete _l coverage, a separate native CRT instance, or the full
  MSVCP locale/facet ABI. Functions not exposed by the bridge, inline guest code
  reading native private layouts, and legacy locale global-data exports remain
  outside this block. Any remaining direct native locale-sensitive route needs
  its own explicit-handle adapter.
- Caller-provided explicit _locale_t handles still require valid ownership.
  The guest remains native code in the host address space, not a memory sandbox.
- Available names/code pages come from the UWP CRT/platform. This does not add
  resource-language selection or MUI fallback.
- No C++/C# build, Bridge fixture execution or Xbox validation was performed.

## Verification

Test-CrtLocale.py passed 34 native UCRT contracts, covering shared/private
threads, aggregate names, pinned localeconv data, explicit buffer/file scanning,
formatting, startup -1, helper-thread snapshot lifetime and unchanged host native
locale/MBCS identities and borrowed pointer contents. The probe avoids calling
native setlocale getters during identity checks because those queries can
modify the very identity being observed. Existing native formatting/scanning
suites passed 13 and 27 contracts. These exercise Windows UCRT, not the bridge.

CrtLocaleFixtures.h exercises the actual helpers/adapters/resolver. It includes
process separation, real shared-thread updates, stale-publication rejection,
concurrent categories, shared/private transitions, explicit/default buffer and
stream locale handling, host private-locale pointer identity, A -> B -> A,
validation callback reentry with retained/released snapshots, controlled
termination while a native _l validation frame is active, and caught SEH. Fixtures are integrated but remain unbuilt/unrun.
Static checks verified SDK/native export names, 282 typed locale/conversion/MBCS adapters,
sorted/unique registrations, XML, Python AST and C++ delimiter balance. A static
CMD import inventory still finds 286 regular plus 19 delay imports with source
registration candidates; it does not prove binding or execution.

References: local ReactOS sdk/lib/ucrt/locale/wsetlocale.cpp, localeconv.cpp,
locale/glstatus.cpp, internal/per_thread_data.cpp and stdio input processing;
installed Windows SDK locale/ctype/stdlib headers; native UCRT reference probes.
Policy/snapshot implementation is bridge translation code with no app-specific
names, metrics or rules. Fatal-handler fallback remains allocation-free;
ordinary guest process/thread snapshot initialization allocates.
