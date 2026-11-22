#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <tuple>
#include <utility>
#include <atomic>
#include "Bridge/CRT/CrtLocale.h"

namespace Win32Bridge { namespace Bridge {
constexpr DWORD CrtGuestTerminationCode = 0xE0425742u;
using CrtExecutionCallback = void(__cdecl*)(void*);
using CrtTerminateHandler = void(__cdecl*)();
using CrtImageBaseResolver = uintptr_t(__cdecl*)(const void*);
// Owned by the guest process and keyed by its executing native thread. This
// survives separate DLL/TLS/UI callback boundaries without changing the host.
using CrtInvalidParameterHandler=void(__cdecl*)(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t);
using CrtSeTranslator=void(__cdecl*)(unsigned,EXCEPTION_POINTERS*);
struct CrtExceptionProcessState {
    std::atomic<CrtInvalidParameterHandler> invalidParameter{nullptr};
    std::atomic<CrtTerminateHandler> purecall{nullptr};
    CrtLocalePolicy locales;
};
struct CrtThreadExceptionState {
    CrtTerminateHandler terminate=nullptr;
    CrtSeTranslator translator=nullptr;
    CrtInvalidParameterHandler invalidParameter=nullptr;
    CrtExceptionProcessState* process=nullptr;
    int errorNumber=0;
    unsigned long dosError=0;
#if defined(_M_X64)
    unsigned int mxcsr = 0x1f80; // Reset rounding, exception masks and status.
#endif
    uint32_t randomSeed=1;
    CrtLocaleThreadState locale;
};
CrtInvalidParameterHandler __cdecl SetCrtInvalidParameterHandler(CrtInvalidParameterHandler);
CrtInvalidParameterHandler __cdecl GetCrtInvalidParameterHandler();
CrtInvalidParameterHandler __cdecl SetCrtThreadInvalidParameterHandler(CrtInvalidParameterHandler);
CrtInvalidParameterHandler __cdecl GetCrtThreadInvalidParameterHandler();
void __cdecl ReportCrtInvalidParameter(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t);
CrtTerminateHandler __cdecl SetCrtPurecallHandler(CrtTerminateHandler);
CrtTerminateHandler __cdecl GetCrtPurecallHandler();
CrtSeTranslator __cdecl SetCrtSeTranslator(CrtSeTranslator);
uintptr_t CrtExceptionPtrHelperAddress(const wchar_t* name);
CrtThreadExceptionState* SetCrtThreadExceptionState(CrtThreadExceptionState* state);
CrtThreadExceptionState* CurrentCrtThreadExceptionState();
void __cdecl SeedCrtRandom(unsigned int seed);
int __cdecl NextCrtRandom();
// Default-locale operations use this explicit handle, never the host PTD.
// During protected execution, the boundary retains all handles used by a call,
// including snapshots superseded by a reentrant validation callback.
_locale_t CurrentCrtLocaleHandle();
unsigned CurrentCrtConsoleSourcePage();
bool SynchronizeCrtLocale();
int __cdecl ConfigureCrtLocale(int mode);
int __cdecl SetCrtMultibyteCodePage(int codePage);
int __cdecl GetCrtMultibyteCodePage();
char* __cdecl SetCrtLocale(int category,const char* name);
wchar_t* __cdecl SetCrtWideLocale(int category,const wchar_t* name);
lconv* __cdecl GetCrtLocaleConvention();
_locale_t __cdecl GetCrtCurrentLocale();

// Installs thread-scoped termination isolation. Hardware faults propagate
// to the caller's SEH boundary; terminate or an unhandled C++ throw returns false.
// The callback and all guest frames still participate in native x64 unwinding.
bool InvokeCrtExecution(CrtExecutionCallback callback, void* context);
__declspec(noreturn) void AbortCrtExecution();
CrtTerminateHandler __cdecl SetCrtTerminateHandler(CrtTerminateHandler handler);
CrtTerminateHandler __cdecl GetCrtTerminateHandler();
__declspec(noreturn) void __cdecl TerminateCrtExecution();

// Registered for the entire RuntimeImage lifetime, including DLL detach.
bool RegisterCrtGuestImage(const void* base, size_t size);
void UnregisterCrtGuestImage(const void* base);
uintptr_t __cdecl CrtGuestImageBase(const void* address);

// ThrowInfo and its type/funclet references are image-relative on x64. Manual
// PE images need the guest image resolver, not the host's module enumeration.
__declspec(noreturn) void __stdcall RaiseCrtException(void* object, void* throwInfo, CrtImageBaseResolver resolveImageBase = &CrtGuestImageBase);
uintptr_t CrtExceptionHelperAddress(const wchar_t* name);
namespace CrtExceptionDetail {
    template<class Result> struct StoreResult
    {
        template<class Function, class... Args> static void Call(Result* output, Function function, Args&&... args)
        { const Result value = function(std::forward<Args>(args)...); if (output) *output = value; }
    };
    template<> struct StoreResult<void>
    {
        template<class Function, class... Args> static void Call(void*, Function function, Args&&... args)
        { function(std::forward<Args>(args)...); }
    };
    template<class Result, class Function, class... Args> struct FunctionCall
    {
        Result* output;
        Function function;
        std::tuple<Args...> arguments;
        template<size_t... Indices> void Apply(std::index_sequence<Indices...>)
        { StoreResult<Result>::Call(output, function, std::get<Indices>(arguments)...); }
        static void __cdecl Invoke(void* context)
        { static_cast<FunctionCall*>(context)->Apply(std::index_sequence_for<Args...>{}); }
    };
}
// Keep all C++ argument bookkeeping outside callers' tiny __try leaves.
template<class Result, class Function, class... Args>
bool InvokeCrtFunction(Result* output, Function function, Args... args)
{
    using Call = CrtExceptionDetail::FunctionCall<Result, Function, Args...>;
    Call call{ output, function, std::tuple<Args...>(args...) };
    return InvokeCrtExecution(&Call::Invoke, &call);
}
} }
