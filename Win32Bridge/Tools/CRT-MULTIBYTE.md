# Guest CRT multibyte state

The bridge now keeps MBCS code-page selection independently of LC_CTYPE.
_setmbcp and _getmbcp use the same process/thread shared/private configuration
as locale, without changing native host globals or the execution thread's PTD.
GuestCodePages.h centralizes the existing guest ANSI 1252 and OEM 437 defaults
used by both Kernel32 and CRT; they are environment defaults, not application
rules. _MB_CP_ANSI and _MB_CP_OEM resolve to those guest values, not the host's.
_MB_CP_LOCALE resolves once from the selected LC_CTYPE in the private helper;
later LC_CTYPE changes do not implicitly change the MBCS code page.

## State and ownership

CrtLocalePolicy owns an independent immutable MBCS token and per-thread private
and observed tokens. A code-page setter compares its expected token before
publication and retries after a peer update. Locale category setters publish
only LC state, and MBCS setters publish only MBCS state. Their independently
changing tokens both participate in the native snapshot cache key. A cached
pair superseded during a concurrent update is refreshed before the next
locale-sensitive operation, rather than overwriting the other component.

Native _getmbcp returns zero for SBCS and UTF-8, so its result cannot reconstruct
the selected page. The snapshot retains both the effective page and the native
reported result. CP1251, CP1252, SBCS and UTF-8 remain distinct even when all
report zero. Native _l functions receive an ordinary native handle containing
both the selected LC categories and the independent MBCS reference. Owned
_get_current_locale clones preserve both components and use native _free_locale.

Snapshot construction applies the category state and _setmbcp on a private
helper thread. Supported pages, invalid-page errors, byte/string semantics and
encoding limitations come from the matching native UCRT. No private CRT layout
or refcount offset is accessed; _MB_CP_LOCALE uses the public SDK-declared
___lc_codepage_func on that helper. No guest input/callback runs on it.

## Lazy initialization and private mode

The first guest CRT thread is initialized from process shared data. Other
threads initially retain the current process LC categories and initial SBCS
MBCS data. They synchronize when a locale-sensitive operation requests a native
snapshot. Merely entering a callback does not cause that synchronization.

_configthreadlocale(1) captures the thread's observed state, rather than forcing
a global refresh. Thus a fresh thread which enables private mode before any
MBCS observation retains initial SBCS data. A thread which reads the shared page
first captures that observed page. Disabling private mode allows a subsequent
operation to synchronize. Startup -1 preserves independent thread state while
leaving the process shared values available to initialize new threads. These
rules follow native observations and the local ReactOS UCRT reference.

The existing protected execution boundary retains handles during reentrant
validation callbacks and releases leases and storage in __finally, including
controlled termination and native SEH. A callback may select another MBCS page
without freeing the handle used by its suspended native _l operation.

## Added routes

- _setmbcp and _getmbcp, including SBCS, ANSI, OEM, LC_CTYPE, DBCS and UTF-8
  selection, invalid-page rejection, and independent shared/private state.
- 78 MBCS operation pairs (156 default/explicit _l routes): byte/character
  classification, lead/trail tests, string length and navigation, byte/character
  counts, comparisons/collation, searches, casing, Japanese conversions, secure
  copies/concatenation and caller-context tokenization.
- 10 conversion pairs (20 default/explicit _l routes): mblen, mbtowc, mbstowcs,
  mbstowcs_s, wctomb, wctomb_s, wcstombs, wcstombs_s, _mbstrlen and _mbstrnlen.
  These follow LC_CTYPE rather than the independent MBCS selection.

Null explicit handles select the guest default. Valid non-null handles retain
native semantics and caller ownership. The old always-zero _ismbblead shim was
removed. This is shared CRT behavior without executable-name checks.

## Verification and limits

Test-CrtMultibyte.py passed 33 native Windows UCRT contracts covering SBCS/DBCS/
UTF-8, distinct SBCS mappings despite _getmbcp()==0, LC_CTYPE independence,
conversion, owned snapshots after helper exit, string operations, invalid pages,
lazy private-thread initialization, shared updates, preserved host identity and
startup -1 in an isolated subprocess. Existing locale, formatting and scanning
reference suites passed 34, 13 and 27 contracts. The total is 107 native reference
checks, not execution of bridge adapters.

CrtMultibyteFixtures.h integrates actual bridge helpers/resolver/adapters. It
covers distinct host state, fresh/private/shared threads, cross-process reentry,
concurrent LC and MBCS updates, stale publication rejection, owned handles,
conversion, guest ANSI/OEM defaults, startup mode and validation callback
reentry with snapshot lifetime checks. It remains unbuilt/unrun at the user's
request. Static checks passed for declarations, native export availability,
282 typed data adapters, sorted/unique registrations, project/header references,
XML, Python AST and delimiter balance. The static CMD inventory still reports
286 regular and 19 delay imports with registration candidates; this does not
prove actual binding, linking or execution on Xbox.

Raw writable _mbctype/_mbcasemap globals and their pointer-export ABI are not
implemented here. Restartable wchar_t conversions now cover mbrtowc, mbrlen,
mbsinit, btowc/wctob, wcrtomb and both directions of string conversion,
including the secure variants. They use guest LC_CTYPE and retain separate
implicit input states per guest thread and entry point. UTF-8 strings keep
surrogate pairs intact at output limits. wcrtomb_s enforces its destination
capacity even where the native CRT writes beyond the supplied size. The C11
char16_t and char32_t conversions delegate to the matching UCRT. These entry
points always use UTF-8 on Windows, with independent implicit state per guest
thread and function. The full locale/facet ABI and UTF-8 edge cases in the
custom stream scanner are separate. Native UCRT limitations of the delegated MBCS routines
are preserved; support for selecting UTF-8 does not make every old DBCS routine
a Unicode code-point iterator. Helper-thread construction costs can be optimized
later. Actual App CRT linking, bridge behavior and Xbox validation remain for
user builds/tests; no C++/C# build was run.

References: local reactos-master/sdk/lib/ucrt/mbstring/mbctype.cpp,
locale/wsetlocale.cpp, internal/per_thread_data.cpp and inc/corecrt_internal.h;
installed Windows SDK mbctype.h/mbstring.h/locale.h/stdlib.h; native Windows
black-box probes; Microsoft [_setmbcp](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmbcp)
and [_getmbcp](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/getmbcp).

UWP declaration correction: mbstring.h hides most memory-only declarations
behind _CRT_USE_WINAPI_FAMILY_DESKTOP_APP. CrtMultibyteData.h now supplies 64
explicit, SDK-matching extern C import declarations only when that desktop
macro is absent. It does not redefine API-family macros or replace operations
with stubs. All 78 called native _l functions are covered by either exposed SDK
declarations or these declarations; their symbols were checked in SDK ucrt.lib.
This source/import-library inventory does not replace an App CRT link or Xbox
runtime test. No build was performed for this correction.
