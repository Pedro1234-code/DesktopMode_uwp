#include "Bridge/CRT/CrtExceptions.h"
#include <csetjmp>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <eh.h>
#include <ehdata.h>
#include <exception>
#include <new>
#include <limits>
#include <iterator>
#include <thread>
#include <system_error>
#include <vector>
#include <mbctype.h>
#include <map>
#include <mutex>
#include <vcruntime_exception.h>
#include <vcruntime_typeinfo.h>
#include <rttidata.h>
#if defined(_M_X64)
#include <xmmintrin.h>
#endif

extern "C" EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler3(PEXCEPTION_RECORD, PVOID, PCONTEXT, PDISPATCHER_CONTEXT);
#if defined(_M_X64) && WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
extern "C" EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler4(PEXCEPTION_RECORD, PVOID, PCONTEXT, PDISPATCHER_CONTEXT);
#endif
extern "C" _VCRTIMP void** __cdecl __current_exception();
extern "C" _VCRTIMP void** __cdecl __current_exception_context();
extern "C" _VCRTIMP int* __cdecl __processing_throw();
extern "C" _VCRTIMP void __cdecl __std_type_info_destroy_list(__type_info_node*);

namespace Win32Bridge { namespace Bridge {
namespace {
    struct GuestImages
    {
        std::mutex lock;
        std::map<uintptr_t, size_t> ranges;
    };
    GuestImages& Images()
    {
        // RuntimeImage owners can be destroyed from late host shutdown code.
        // Keep the registry itself alive until process teardown; individual
        // image records are still removed promptly by RuntimeImage::Release.
        static GuestImages* const images = new GuestImages;
        return *images;
    }
#if defined(_M_X64)
    uintptr_t FrameHandler4Address()
    {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        // Native /MT fixtures must use the same static CRT as their catches.
        return reinterpret_cast<uintptr_t>(&::__CxxFrameHandler4);
#else
        // An unconditional FH4 import would prevent the entire runtime DLL
        // from loading on a device with an older VCLibs framework. Resolve the
        // matching App CRT satellite from the package graph when requested.
        // Never mix its per-thread EH state with desktop vcruntime140.dll.
        static const uintptr_t address = []() -> uintptr_t
        {
            // Keep our own module reference for the lifetime of the IAT pointers.
#if defined(_DEBUG)
            HMODULE module = ::LoadPackagedLibrary(L"vcruntime140_1d_app.dll", 0);
#else
            HMODULE module = ::LoadPackagedLibrary(L"vcruntime140_1_app.dll", 0);
#endif
            return module ? reinterpret_cast<uintptr_t>(::GetProcAddress(module, "__CxxFrameHandler4")) : 0;
        }();
        return address;
#endif
    }
#endif
    uintptr_t IntrinsicSetjmpAddress()
    {
        // Return the CRT entry itself: a C++ wrapper would capture its own
        // stack frame and leave the guest jump buffer invalid after returning.
        // MSVC treats _setjmp as intrinsic-only, so its address cannot be taken.
        static const uintptr_t address = []() -> uintptr_t
        {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#if defined(_DEBUG)
            HMODULE module = ::LoadLibraryW(L"vcruntime140d.dll");
#else
            HMODULE module = ::LoadLibraryW(L"vcruntime140.dll");
#endif
#else
#if defined(_DEBUG)
            HMODULE module = ::LoadPackagedLibrary(L"vcruntime140d_app.dll", 0);
#else
            HMODULE module = ::LoadPackagedLibrary(L"vcruntime140_app.dll", 0);
#endif
#endif
            // Keep the reference alive for every guest IAT using this address.
            return module ? reinterpret_cast<uintptr_t>(::GetProcAddress(module, "__intrinsic_setjmp")) : 0;
        }();
        return address;
    }
    struct ExecutionBoundary
    {
        std::jmp_buf target;
        ExecutionBoundary* previous;
        CrtThreadExceptionState* guestState;
        volatile bool* terminated;
        CrtTerminateHandler previousTerminate;
        CrtTerminateHandler previousGuestTerminate;
        void** exceptionSlot;
        void** contextSlot;
        int* processingSlot;
        void* previousException;
        void* previousContext;
        int previousProcessing;
        CrtInvalidParameterHandler previousInvalidParameter;
        CrtSeTranslator previousTranslator;
        int* errorSlot;
        unsigned long* dosErrorSlot;
        int previousError;
        unsigned long previousDosError;
        bool isolatesErrors;
#if defined(_M_X64)
        unsigned int previousMxcsr;
#endif
    };
    // Trivial TLS only: dynamic thread_local initialization is unavailable /ZW.
    thread_local ExecutionBoundary* g_executionBoundary = nullptr;
    thread_local CrtTerminateHandler g_guestTerminate = nullptr;
    thread_local CrtThreadExceptionState* g_guestThreadExceptions = nullptr;
    thread_local CrtSeTranslator g_guestTranslator=nullptr;
    thread_local CrtInvalidParameterHandler g_guestInvalidParameter=nullptr;
    thread_local uint32_t g_fallbackRandomSeed=1;
    std::atomic<CrtInvalidParameterHandler>& InvalidParameterSlot() {
        if(g_guestThreadExceptions&&g_guestThreadExceptions->process)return g_guestThreadExceptions->process->invalidParameter;
        static std::atomic<CrtInvalidParameterHandler> fallback{nullptr};return fallback;
    }
    std::atomic<CrtTerminateHandler>& PurecallSlot() {
        if(g_guestThreadExceptions&&g_guestThreadExceptions->process)return g_guestThreadExceptions->process->purecall;
        static std::atomic<CrtTerminateHandler> fallback{nullptr};return fallback;
    }
#pragma warning(push)
#pragma warning(disable:4535)
    CrtSeTranslator ExchangeNativeTranslator(CrtSeTranslator translator){return ::_set_se_translator(translator);}
#pragma warning(pop)
    CrtTerminateHandler& GuestTerminateSlot()
    { return g_guestThreadExceptions ? g_guestThreadExceptions->terminate : g_guestTerminate; }

    __declspec(noreturn) void __cdecl DefaultGuestTerminate() { AbortCrtExecution(); }
    __declspec(noreturn) void __cdecl DispatchGuestTerminate()
    {
        // UCRT aborts if a user terminate handler returns OR raises an SEH
        // exception. Catch it here and leave via the controlled jump instead.
        const auto handler = GuestTerminateSlot();
        if (handler && handler != &DispatchGuestTerminate && handler != &DefaultGuestTerminate)
        {
            __try { handler(); }
            __except(EXCEPTION_EXECUTE_HANDLER) { }
        }
        AbortCrtExecution();
    }
    // Binary layout used by C++/CX WinRT exception preparation. Public SDK
    // ThrowInfo supplies the type attributes; this prefix is pointer-sized,
    // unlike the 4-byte-packed image-relative ThrowInfo itself.
    struct WinRtThrowPreparation
    {
        void* description;
        void* restrictedErrorString;
        void* restrictedErrorReference;
        void* capabilitySid;
        long hresult;
        void* restrictedInfo;
        const ThrowInfo* throwInfo;
        unsigned size;
        void(__stdcall* prepare)(void*);
    };
#if defined(_M_X64)
    static_assert(offsetof(WinRtThrowPreparation, throwInfo) == 48, "WinRT EH metadata layout");
    static_assert(offsetof(WinRtThrowPreparation, prepare) == 64, "WinRT EH preparation layout");
    static_assert(sizeof(ThrowInfo) == 16, "x64 relative ThrowInfo layout");
#endif
}

#include "Bridge/CRT/CrtLocale.inl"

bool RegisterCrtGuestImage(const void* image, size_t size)
{
    const auto base = reinterpret_cast<uintptr_t>(image);
    if (!base || !size || size > (std::numeric_limits<uintptr_t>::max)() - base) return false;
    try
    {
        auto& images = Images(); std::lock_guard<std::mutex> guard(images.lock);
        const auto next = images.ranges.lower_bound(base);
        if (next != images.ranges.end() && next->first == base) return next->second == size;
        if (next != images.ranges.end() && next->first - base < size) return false;
        if (next != images.ranges.begin())
        {
            const auto previous = std::prev(next);
            if (base - previous->first < previous->second) return false;
        }
        images.ranges.emplace(base, size);
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
}
void UnregisterCrtGuestImage(const void* image)
{
    auto& images = Images(); std::lock_guard<std::mutex> guard(images.lock);
    images.ranges.erase(reinterpret_cast<uintptr_t>(image));
}
uintptr_t __cdecl CrtGuestImageBase(const void* address)
{
    const auto pc = reinterpret_cast<uintptr_t>(address);
    auto& images = Images(); std::lock_guard<std::mutex> guard(images.lock);
    auto next = images.ranges.upper_bound(pc);
    if (next == images.ranges.begin()) return 0;
    --next;
    return pc - next->first < next->second ? next->first : 0;
}
static __declspec(noinline) bool InvokeCrtExecutionLeaf(CrtExecutionCallback callback, void* context, CrtLocaleBoundary* locale)
{
    if (!callback) return false;
    volatile bool terminated = false;
    ExecutionBoundary state{};
    state.previous = g_executionBoundary;
    state.guestState = g_guestThreadExceptions;
    state.terminated = &terminated;
    state.previousGuestTerminate = g_guestTerminate;
    state.errorSlot=::_errno();state.dosErrorSlot=::__doserrno();
    state.previousError=*state.errorSlot;state.previousDosError=*state.dosErrorSlot;
    state.isolatesErrors=!state.previous||state.previous->guestState!=state.guestState;
    if(state.isolatesErrors&&state.previous&&state.previous->guestState){
        state.previous->guestState->errorNumber=state.previousError;
        state.previous->guestState->dosError=state.previousDosError;
    }
    state.exceptionSlot = ::__current_exception(); state.previousException = *state.exceptionSlot;
    state.contextSlot = ::__current_exception_context(); state.previousContext = *state.contextSlot;
    state.processingSlot = ::__processing_throw(); state.previousProcessing = *state.processingSlot;
    state.previousTerminate = ::set_terminate(&DispatchGuestTerminate);
    state.previousInvalidParameter = ::_set_thread_local_invalid_parameter_handler(&ReportCrtInvalidParameter);
    state.previousTranslator = ExchangeNativeTranslator(g_guestThreadExceptions?g_guestThreadExceptions->translator:g_guestTranslator);
    // Do not expose a host/other-guest active catch to current_exception() or
    // a guest rethrow. Reentry into this same guest thread must retain its
    // active exception, as ordinary nested Win32 callbacks do.
    if (!state.previous || state.previous->guestState != state.guestState)
    {
        *state.exceptionSlot = nullptr;
        *state.contextSlot = nullptr;
        *state.processingSlot = 0;
    }
    if(state.isolatesErrors){
        *state.errorSlot=state.guestState?state.guestState->errorNumber:0;
        *state.dosErrorSlot=state.guestState?state.guestState->dosError:0;
    }else{
        *state.errorSlot=state.previousError;*state.dosErrorSlot=state.previousDosError;
    }
    if (!state.previous && !g_guestThreadExceptions) g_guestTerminate = nullptr;
#if defined(_M_X64)
    state.previousMxcsr = _mm_getcsr();
    if (state.isolatesErrors)
    {
        if (state.previous && state.previous->guestState)
            state.previous->guestState->mxcsr = state.previousMxcsr;
        if (state.guestState) _mm_setcsr(state.guestState->mxcsr);
    }
#endif
    g_executionBoundary = &state;
    const int jumped = setjmp(state.target);
    bool completed = false;
    bool localeRestored = true;
    // This leaf intentionally contains no C++ objects requiring destructors.
    // __finally also restores TLS when ExitProcess/ExitThread jumps past us.
    __try
    {
        if (!jumped) { InvokeCxxBody(callback, context, locale); completed = true; }
    }
    __finally
    {
        const int guestError=*state.errorSlot;
        const unsigned long guestDosError=*state.dosErrorSlot;
#if defined(_M_X64)
        const unsigned int guestMxcsr = _mm_getcsr();
        if (state.guestState) state.guestState->mxcsr = guestMxcsr;
#endif
        if(state.guestState){state.guestState->errorNumber=guestError;state.guestState->dosError=guestDosError;}
        if(locale)localeRestored=locale->Restore();
        ExchangeNativeTranslator(state.previousTranslator);
        ::_set_thread_local_invalid_parameter_handler(state.previousInvalidParameter);
        ::set_terminate(state.previousTerminate);
        *state.exceptionSlot = state.previousException;
        *state.contextSlot = state.previousContext;
        *state.processingSlot = state.previousProcessing;
        if(state.isolatesErrors){
            // A -> B -> A reentry may update A while B is active. Restore the
            // latest suspended guest state rather than B's stale snapshot.
            const auto suspended=state.previous?state.previous->guestState:nullptr;
            *state.errorSlot=suspended?suspended->errorNumber:state.previousError;
            *state.dosErrorSlot=suspended?suspended->dosError:state.previousDosError;
        }else{*state.errorSlot=guestError;*state.dosErrorSlot=guestDosError;}
        g_executionBoundary = state.previous;
        // A nested DLL callback shares this guest thread's user handler.
        if (!state.previous) g_guestTerminate = state.previousGuestTerminate;
#if defined(_M_X64)
        const auto suspended = state.previous ? state.previous->guestState : nullptr;
        _mm_setcsr(state.isolatesErrors ? (suspended ? suspended->mxcsr : state.previousMxcsr) : guestMxcsr);
#endif
    }
    return completed && !terminated && localeRestored;
}

bool InvokeCrtExecution(CrtExecutionCallback callback, void* context)
{
    if(!callback)return false;
    const int saved=errno;unsigned long savedDos=0;::_get_doserrno(&savedDos);
    try{
        CrtLocaleBoundary locale(g_guestThreadExceptions,g_executionBoundary?g_executionBoundary->guestState:nullptr);
        ::_set_doserrno(savedDos);errno=saved;
        // Snapshot leases live outside the setjmp target. Their release is
        // explicit in the leaf's __finally, including native SEH fault paths.
        return InvokeCrtExecutionLeaf(callback,context,&locale);
    }catch(const std::bad_alloc&){::_set_doserrno(savedDos);errno=saved;return false;}
}

__declspec(noreturn) void AbortCrtExecution()
{
    if (g_executionBoundary)
    {
        *g_executionBoundary->terminated = true;
        longjmp(g_executionBoundary->target, 1);
    }
    // No registered native terminate handler points here outside a boundary.
    // Preserve controlled-failure behavior for an explicitly invoked shim.
    for (;;) ::RaiseException(CrtGuestTerminationCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
CrtThreadExceptionState* CurrentCrtThreadExceptionState(){return g_guestThreadExceptions;}
void __cdecl SeedCrtRandom(unsigned int seed){
    auto& value=g_guestThreadExceptions?g_guestThreadExceptions->randomSeed:g_fallbackRandomSeed;value=seed;
}
int __cdecl NextCrtRandom(){
    auto& value=g_guestThreadExceptions?g_guestThreadExceptions->randomSeed:g_fallbackRandomSeed;
    value=value*214013u+2531011u;return static_cast<int>((value>>16)&0x7fffu);
}
CrtThreadExceptionState* SetCrtThreadExceptionState(CrtThreadExceptionState* state)
{
    const auto previous = g_guestThreadExceptions;
    if(state&&state->process)state->process->locales.InitializeThread(state->locale);
    g_guestThreadExceptions = state;
    return previous;
}
CrtTerminateHandler __cdecl SetCrtTerminateHandler(CrtTerminateHandler handler)
{
    const auto previous = GetCrtTerminateHandler();
    GuestTerminateSlot() = handler == &DefaultGuestTerminate ? nullptr : handler;
    return previous;
}
CrtTerminateHandler __cdecl GetCrtTerminateHandler()
{ return GuestTerminateSlot() ? GuestTerminateSlot() : &DefaultGuestTerminate; }
__declspec(noreturn) void __cdecl TerminateCrtExecution() { DispatchGuestTerminate(); }

__declspec(noreturn) void __stdcall RaiseCrtException(void* object, void* metadata, CrtImageBaseResolver resolveImageBase)
{
#if defined(_M_X64)
    if (metadata && resolveImageBase && resolveImageBase(metadata))
    {
        auto info = static_cast<const ThrowInfo*>(metadata);
        if (info->attributes & TI_IsWinRT)
        {
            auto preparation = *static_cast<WinRtThrowPreparation***>(object) - 1;
            info = (*preparation)->throwInfo;
            (*preparation)->prepare(preparation);
        }
        const uintptr_t base = info ? resolveImageBase(info) : 0;
        if (base)
        {
            const ULONG_PTR parameters[] = {
                static_cast<ULONG_PTR>((info->attributes & TI_IsPure) ? EH_PURE_MAGIC_NUMBER1 : EH_MAGIC_NUMBER1),
                reinterpret_cast<ULONG_PTR>(object), reinterpret_cast<ULONG_PTR>(info), base
            };
            // Match MSVC/ReactOS's native 4-parameter x64 C++ exception. In
            // particular, never mark a manually mapped native PE as a pure CLR
            // throw merely because RtlPcToFileHeader cannot see its image.
            ::RaiseException(EH_EXCEPTION_NUMBER, EXCEPTION_NONCONTINUABLE, 4, parameters);
            AbortCrtExecution();
        }
        // MSVC typedefs ThrowInfo as a const struct, so casting back to
        // ThrowInfo* does not remove const. Erase the type before converting
        // to the opaque native ABI argument; the metadata remains read-only.
        metadata = const_cast<void*>(static_cast<const void*>(info));
    }
#endif
    // Native metadata and rethrows use the native implementation, including
    // its WinRT preparation and current-exception handling.
    ::_CxxThrowException(object, reinterpret_cast<_ThrowInfo*>(metadata));
}

CrtInvalidParameterHandler __cdecl SetCrtInvalidParameterHandler(CrtInvalidParameterHandler handler){return InvalidParameterSlot().exchange(handler);}
CrtInvalidParameterHandler __cdecl GetCrtInvalidParameterHandler(){return InvalidParameterSlot().load();}
CrtInvalidParameterHandler __cdecl GetCrtThreadInvalidParameterHandler(){return g_guestThreadExceptions?g_guestThreadExceptions->invalidParameter:g_guestInvalidParameter;}
CrtInvalidParameterHandler __cdecl SetCrtThreadInvalidParameterHandler(CrtInvalidParameterHandler handler){
    auto& slot=g_guestThreadExceptions?g_guestThreadExceptions->invalidParameter:g_guestInvalidParameter;auto previous=slot;slot=handler;return previous;
}
void __cdecl ReportCrtInvalidParameter(const wchar_t* expression,const wchar_t* function,const wchar_t* file,unsigned line,uintptr_t reserved){
    auto handler=GetCrtThreadInvalidParameterHandler();if(!handler)handler=GetCrtInvalidParameterHandler();
    if(handler){handler(expression,function,file,line,reserved);return;}TerminateCrtExecution();
}
CrtTerminateHandler __cdecl SetCrtPurecallHandler(CrtTerminateHandler handler){return PurecallSlot().exchange(handler);}
CrtTerminateHandler __cdecl GetCrtPurecallHandler(){return PurecallSlot().load();}
CrtSeTranslator __cdecl SetCrtSeTranslator(CrtSeTranslator translator){
    auto& slot=g_guestThreadExceptions?g_guestThreadExceptions->translator:g_guestTranslator;
    auto previous=slot;slot=translator;if(g_executionBoundary)ExchangeNativeTranslator(translator);return previous;
}
void __cdecl GuestInvalidParameterNoInfo(){ReportCrtInvalidParameter(nullptr,nullptr,nullptr,0,0);}
__declspec(noreturn) void __cdecl GuestInvalidParameterNoReturn(){GuestInvalidParameterNoInfo();TerminateCrtExecution();}
__declspec(noreturn) void __cdecl GuestInvokeWatson(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){TerminateCrtExecution();}
int __cdecl GuestPurecall(){if(auto handler=GetCrtPurecallHandler())handler();TerminateCrtExecution();}

void __cdecl GuestLocalUnwind(void* frame, void* target)
{ ::RtlUnwind(frame, target, nullptr, nullptr); }

#include "Bridge/CRT/CrtExceptionPtr.inl"

uintptr_t CrtExceptionHelperAddress(const wchar_t* name)
{
    if (!name) return 0;
#define CRT_EH_EXPORT(exportName, function) if (!std::wcscmp(name, L##exportName)) return reinterpret_cast<uintptr_t>(&function)
    CRT_EH_EXPORT("__CxxFrameHandler", ::__CxxFrameHandler3);
    CRT_EH_EXPORT("__CxxFrameHandler2", ::__CxxFrameHandler3);
    CRT_EH_EXPORT("__CxxFrameHandler3", ::__CxxFrameHandler3);
#if defined(_M_X64)
    if (!std::wcscmp(name, L"__CxxFrameHandler4")) return FrameHandler4Address();
#endif
    if (!std::wcscmp(name, L"__intrinsic_setjmp")) return IntrinsicSetjmpAddress();
    CRT_EH_EXPORT("longjmp", ::longjmp);
    CRT_EH_EXPORT("_local_unwind", GuestLocalUnwind);
    CRT_EH_EXPORT("terminate", TerminateCrtExecution);
    CRT_EH_EXPORT("?terminate@@YAXXZ", TerminateCrtExecution);
    CRT_EH_EXPORT("__std_terminate", TerminateCrtExecution);
    CRT_EH_EXPORT("_purecall", GuestPurecall);
    CRT_EH_EXPORT("_set_purecall_handler", SetCrtPurecallHandler);
    CRT_EH_EXPORT("_get_purecall_handler", GetCrtPurecallHandler);
    CRT_EH_EXPORT("_set_invalid_parameter_handler", SetCrtInvalidParameterHandler);
    CRT_EH_EXPORT("_get_invalid_parameter_handler", GetCrtInvalidParameterHandler);
    CRT_EH_EXPORT("_set_thread_local_invalid_parameter_handler", SetCrtThreadInvalidParameterHandler);
    CRT_EH_EXPORT("_get_thread_local_invalid_parameter_handler", GetCrtThreadInvalidParameterHandler);
    CRT_EH_EXPORT("_invalid_parameter", ReportCrtInvalidParameter);
    CRT_EH_EXPORT("_invalid_parameter_noinfo", GuestInvalidParameterNoInfo);
    CRT_EH_EXPORT("_invalid_parameter_noinfo_noreturn", GuestInvalidParameterNoReturn);
    CRT_EH_EXPORT("_invoke_watson", GuestInvokeWatson);
    CRT_EH_EXPORT("_set_se_translator", SetCrtSeTranslator);
    CRT_EH_EXPORT("set_terminate", SetCrtTerminateHandler);
    CRT_EH_EXPORT("?set_terminate@@YAP6AXXZP6AXXZ@Z", SetCrtTerminateHandler);
    CRT_EH_EXPORT("_get_terminate", GetCrtTerminateHandler);
    CRT_EH_EXPORT("__current_exception", ::__current_exception);
    CRT_EH_EXPORT("__current_exception_context", ::__current_exception_context);
    CRT_EH_EXPORT("__processing_throw", ::__processing_throw);
    CRT_EH_EXPORT("__uncaught_exception", ::__uncaught_exception);
    CRT_EH_EXPORT("__uncaught_exceptions", ::__uncaught_exceptions);
    CRT_EH_EXPORT("__RTDynamicCast", ::__RTDynamicCast);
    CRT_EH_EXPORT("__RTtypeid", ::__RTtypeid);
    CRT_EH_EXPORT("__RTCastToVoid", ::__RTCastToVoid);
    CRT_EH_EXPORT("_is_exception_typeof", ::_is_exception_typeof);
    CRT_EH_EXPORT("__std_exception_copy", ::__std_exception_copy);
    CRT_EH_EXPORT("__std_exception_destroy", ::__std_exception_destroy);
    CRT_EH_EXPORT("__std_type_info_compare", ::__std_type_info_compare);
    CRT_EH_EXPORT("__std_type_info_hash", ::__std_type_info_hash);
    CRT_EH_EXPORT("__std_type_info_name", ::__std_type_info_name);
    CRT_EH_EXPORT("__std_type_info_destroy_list", ::__std_type_info_destroy_list);
#undef CRT_EH_EXPORT
    return 0;
}
} }
