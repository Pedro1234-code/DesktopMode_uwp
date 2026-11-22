#include "pch.h"
#include "Bridge/CRT/CrtExceptions.h"
#include "Bridge/CRT/MsvcrtShims.h"
#include "Bridge/CRT/CrtData.h"
#include "Bridge/CRT/CrtFormatting.h"
#include "Bridge/CRT/CrtScanning.h"
#include "Bridge/CRT/CrtStdioScanInput.h"
#include "Bridge/GuestModule.h"
#include "Bridge/GuestRuntime.h"
#include "Bridge/GuestKernel.h"
#include <locale.h>
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <cstdlib>
#include <algorithm>
#include <limits>
#include <cstdarg>
#include <corecrt_startup.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

extern "C" EXCEPTION_DISPOSITION __cdecl __C_specific_handler(
    PEXCEPTION_RECORD exceptionRecord,
    PVOID establisherFrame,
    PCONTEXT contextRecord,
    PDISPATCHER_CONTEXT dispatcherContext);

namespace
{
    Win32Bridge::Bridge::CrtProcessState* CurrentCrt()
    {
        auto loader = Win32Bridge::Bridge::CurrentGuestModuleLoader();
        return loader ? &loader->CrtState() : nullptr;
    }
    int ConfigureCrtArguments(bool wide, int mode);
    constexpr DWORD GuestCxxExceptionCode = 0xE0425742; // "BWB" bridge exception.

    bool IsMsvcrt(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"msvcrt.dll") == 0;
    }

    int __cdecl BridgeStrcmp(const char* left, const char* right) { return std::strcmp(left ? left : "", right ? right : ""); }
    int __cdecl BridgeWcscmp(const wchar_t* left, const wchar_t* right) { return std::wcscmp(left ? left : L"", right ? right : L""); }
    wchar_t* __cdecl BridgeWcsstr(const wchar_t* text, const wchar_t* pattern) { return const_cast<wchar_t*>(std::wcsstr(text ? text : L"", pattern ? pattern : L"")); }
    void* __cdecl BridgeMalloc(size_t size) { return std::malloc(size); }
    void __cdecl BridgeFree(void* memory) { std::free(memory); }
    void* __cdecl BridgeRealloc(void* memory, size_t size) { return std::realloc(memory, size); }
    void* __cdecl BridgeMemset(void* destination, int value, size_t count) { return std::memset(destination, value, count); }
    size_t __cdecl BridgeStrlen(const char* text) { return text ? std::strlen(text) : 0; }
    char* __cdecl BridgeStrchr(const char* text, int character) { return const_cast<char*>(std::strchr(text ? text : "", character)); }
    char* __cdecl BridgeStrstr(const char* text, const char* pattern) { return const_cast<char*>(std::strstr(text ? text : "", pattern ? pattern : "")); }
    size_t __cdecl BridgeWcslen(const wchar_t* text) { return text ? std::wcslen(text) : 0; }
    int __cdecl BridgeMemcmp(const void* left, const void* right, size_t count) { return std::memcmp(left, right, count); }
    void* __cdecl BridgeMemmove(void* destination, const void* source, size_t count) { return std::memmove(destination, source, count); }
    void* __cdecl BridgeMemcpy(void* destination, const void* source, size_t count) { return std::memcpy(destination, source, count); }



    uintptr_t __cdecl BridgeBeginThreadEx(
        void*,
        unsigned stackSize,
        unsigned(__stdcall* startAddress)(void*),
        void* parameter,
        unsigned creationFlags,
        unsigned* threadId)
    {
        DWORD id = 0;
        HANDLE handle = Win32Bridge::Bridge::BridgeCreateThread(
            nullptr,
            stackSize,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(startAddress),
            parameter,
            creationFlags,
            &id);
        if (threadId) *threadId = id;
        return reinterpret_cast<uintptr_t>(handle);
    }
    EXCEPTION_DISPOSITION __cdecl BridgeCSpecificHandler(
        PEXCEPTION_RECORD exceptionRecord,
        PVOID establisherFrame,
        PCONTEXT contextRecord,
        PDISPATCHER_CONTEXT dispatcherContext)
    {
        // x64 scope tables are interpreted by the platform handler.  Returning
        // ContinueSearch unconditionally bypasses every guest __try/__except
        // and __finally block and can turn an intentionally guarded probe into
        // a process-level failure.
        return ::__C_specific_handler(
            exceptionRecord,
            establisherFrame,
            contextRecord,
            dispatcherContext);
    }
    __declspec(noreturn) void __stdcall BridgeCxxThrowException(void* object, void* throwInfo)
    { Win32Bridge::Bridge::RaiseCrtException(object, throwInfo); }
    void __cdecl BridgeTypeInfoDestructor() { }
    __declspec(noreturn) void __cdecl BridgeTerminate()
    { Win32Bridge::Bridge::TerminateCrtExecution(); }
    void __cdecl BridgeImmediateExit(int code)
    {
        if (auto state = CurrentCrt()) state->suppressStdioFlush.store(true);
        if (!Win32Bridge::Bridge::RequestGuestExit(static_cast<DWORD>(code), false))
            RaiseException(GuestCxxExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    void __cdecl BridgeExit(int code)
    {
        if (auto state = CurrentCrt()) state->Execute();
        if (auto loader = Win32Bridge::Bridge::CurrentGuestModuleLoader()) loader->Stdio().FlushAll(false);
        if (!Win32Bridge::Bridge::RequestGuestExit(static_cast<DWORD>(code), false))
            RaiseException(GuestCxxExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    void __cdecl BridgeEndThreadEx(unsigned code)
    {
        if (!Win32Bridge::Bridge::RequestGuestExit(code, true))
            RaiseException(GuestCxxExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    int __cdecl BridgeInitTermE(int(__cdecl** first)(), int(__cdecl** last)())
    {
        if (first == last) return 0;
        if (!first || !last || first > last) return EINVAL;
        for (auto current = first; current != last; ++current)
        {
            if (!*current) continue;
            const int result = (*current)();
            if (result) return result;
        }
        return 0;
    }
    void __cdecl BridgeCExit()
    {
        if (auto state = CurrentCrt()) state->Execute();
        if (auto loader = Win32Bridge::Bridge::CurrentGuestModuleLoader()) loader->Stdio().FlushAll(false);
    }
    void __cdecl BridgeQuickCExit() { }
    int(__cdecl* BridgeOnExit(int(__cdecl* callback)()))()
    {
        if (!callback) { errno = EINVAL; return nullptr; }
        auto state = CurrentCrt();
        if (!state || !state->Register(reinterpret_cast<Win32Bridge::Bridge::CrtProcessState::ExitCallback>(callback)))
        { errno = ENOMEM; return nullptr; }
        return callback;
    }
    int __cdecl BridgeAtExit(void(__cdecl* callback)())
    {
        if (!callback) { errno = EINVAL; return -1; }
        auto state = CurrentCrt();
        if (state && state->Register(callback)) return 0;
        errno = ENOMEM; return -1;
    }
    int __cdecl BridgeXcptFilter(unsigned long, PEXCEPTION_POINTERS) { return EXCEPTION_CONTINUE_SEARCH; }
    int __cdecl BridgeGetMainArgs(int* argumentCount, char*** argumentValues, char*** environment, int expandWildcards, int*)
    {
        auto state = CurrentCrt();
        if (!state || !argumentCount || !argumentValues || !environment) return EINVAL;
        const int status = ConfigureCrtArguments(false, expandWildcards ? 2 : 1);
        if (status) return status;
        *argumentCount = state->argumentCount;
        *argumentValues = state->narrowArguments;
        *environment = state->narrowEnvironment;
        return 0;
    }
    int __cdecl BridgeWGetMainArgs(int* argumentCount, wchar_t*** argumentValues, wchar_t*** environment, int expandWildcards, int*)
    {
        auto state = CurrentCrt();
        if (!state || !argumentCount || !argumentValues || !environment) return EINVAL;
        const int status = ConfigureCrtArguments(true, expandWildcards ? 2 : 1);
        if (status) return status;
        *argumentCount = state->argumentCount;
        *argumentValues = state->wideArguments;
        *environment = state->wideEnvironment;
        return 0;
    }
    void __cdecl BridgeInitTerm(void(__cdecl** first)(), void(__cdecl** last)())
    {
        if (!first || !last || first > last) return;
        for (auto current = first; current != last; ++current) if (*current) (*current)();
    }
    void __cdecl BridgeSetUserMathErr(Win32Bridge::Bridge::CrtProcessState::MathErrorHandler handler)
    {
        if (const auto crt = CurrentCrt()) crt->mathErrorHandler.store(handler);
    }

    int __cdecl BridgeInvokeMathErr(int type, char* name, double arg1, double arg2, double result)
    {
        const auto crt = CurrentCrt();
        const auto handler = crt ? crt->mathErrorHandler.load() : nullptr;
        if (!handler) return 0;
        _exception error{type, name, arg1, arg2, result};
        return handler(&error);
    }
    void __cdecl BridgeSetAppType(int type) { if (auto state = CurrentCrt()) state->appType = type; }
    void __cdecl BridgeSrand(unsigned int seed) { Win32Bridge::Bridge::SeedCrtRandom(seed); }
    int __cdecl BridgeRand() { return Win32Bridge::Bridge::NextCrtRandom(); }

    long __cdecl BridgeWtol(const wchar_t* text) { return Win32Bridge::Bridge::CrtLocaleData__wtol(text); }
    int __cdecl BridgeIswctype(wint_t character, wctype_t type) { return ::_iswctype_l(character, type, Win32Bridge::Bridge::CurrentCrtLocaleHandle()); }
    wchar_t* __cdecl BridgeWcsrchr(const wchar_t* text, wchar_t character)
    {
        return const_cast<wchar_t*>(wcsrchr(text ? text : L"", character));
    }
    int __cdecl BridgeWcsncmp(const wchar_t* left, const wchar_t* right, size_t count)
    {
        return wcsncmp(left ? left : L"", right ? right : L"", count);
    }
    void __cdecl BridgeAmsgExit(int error)
    {
        Win32Bridge::Bridge::RuntimeDiagnostics::Record(
            L"CRT: _amsg_exit(" + std::to_wstring(error) + L").");
        BridgeImmediateExit(255);
    }

}

#include "Bridge/CRT/CrtStartup.inl"
#include "Bridge/CRT/CrtFileShims.inl"
#include "Bridge/CRT/CrtStdioShims.inl"
#include "Bridge/CRT/CrtFormattingShims.inl"
#include "Bridge/CRT/CrtScanningShims.inl"
#include "Bridge/CRT/CrtConsoleProcess.inl"
#include "Bridge/CRT/CrtSpawnShims.inl"
#include "Bridge/CRT/CrtHeapShims.inl"
#include "Bridge/CRT/CrtLocaleExports.inl"

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveMsvcrtImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal) return resolution;
    const bool msvcp = _wcsicmp(symbol.library.c_str(), L"msvcp140.dll") == 0 || _wcsicmp(symbol.library.c_str(), L"msvcp140_app.dll") == 0;
    if (msvcp)
    {
        const auto address = CrtExceptionPtrHelperAddress(symbol.name.c_str());
        return address ? ImportResolution{ImportDisposition::NeedsBridge, L"MSVC exception_ptr ABI with guest throw-image metadata.", address} : resolution;
    }
    const bool vcruntime = _wcsicmp(symbol.library.c_str(), L"vcruntime140.dll") == 0 ||
        _wcsicmp(symbol.library.c_str(), L"vcruntime140_1.dll") == 0 ||
        _wcsicmp(symbol.library.c_str(), L"vcruntime140_app.dll") == 0 ||
        _wcsicmp(symbol.library.c_str(), L"vcruntime140_1_app.dll") == 0;
    if (vcruntime || IsMsvcrt(symbol.library) || _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll") == 0)
    {
        ULONGLONG target = CrtExceptionHelperAddress(symbol.name.c_str());
        if (symbol.name == L"_CxxThrowException") target = reinterpret_cast<ULONGLONG>(&BridgeCxxThrowException);
        else if (symbol.name == L"__C_specific_handler") target = reinterpret_cast<ULONGLONG>(&BridgeCSpecificHandler);
        // VCRUNTIME has a small memory-operation surface, not UCRT's file,
        // process, locale, or global-state APIs. Never alias the whole DLL.
        else if (vcruntime && (symbol.name == L"memcpy" || symbol.name == L"memmove" || symbol.name == L"memset" || symbol.name == L"memcmp"))
        {
            ImportedSymbol data = symbol; data.library = L"ucrtbase.dll";
            target = ResolveCrtDataImport(data).targetAddress;
        }
        if (!target && symbol.name == L"__CxxFrameHandler4")
            RuntimeDiagnostics::Record(L"C++ ABI: __CxxFrameHandler4 requires the matching App CRT FH4 satellite in the host package graph (vcruntime140_1_app.dll; Debug uses vcruntime140_1d_app.dll).");
        if (target) return {ImportDisposition::NeedsBridge, L"MSVC C++ ABI with guest image RVAs and isolated termination.", target};
    }
    if (IsMsvcrt(symbol.library) || _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll") == 0)
    {
        const auto process = ResolveCrtSpawnAddress(symbol.name);
        if (process) return {ImportDisposition::NeedsBridge, L"Guest CRT process creation and wait adapter.", process};
        const auto extra = ResolveCrtConsoleProcess(symbol);
        if (extra.targetAddress) return extra;
    }
    const auto localeResolution = ResolveCrtLocaleImport(symbol);
    if (localeResolution.targetAddress) return localeResolution;
    const auto heapResolution = ResolveCrtHeapImport(symbol);
    if (heapResolution.targetAddress) return heapResolution;
    const auto scanResolution = ResolveCrtScanningImport(symbol);
    if (scanResolution.targetAddress) return scanResolution;
    const auto formatResolution = ResolveCrtFormattingImport(symbol);
    if (formatResolution.targetAddress) return formatResolution;
    const auto stdioResolution = ResolveCrtStdioImport(symbol);
    if (stdioResolution.targetAddress) return stdioResolution;
    const auto fileResolution = ResolveCrtFileImport(symbol);
    if (fileResolution.targetAddress) return fileResolution;
    const auto processResolution = ResolveCrtProcessImport(symbol);
    if (processResolution.targetAddress) return processResolution;
    const bool ucrt = _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll") == 0;
    if (IsMsvcrt(symbol.library) || ucrt)
    {
        if (symbol.name == L"__setusermatherr")
            return {ImportDisposition::NeedsBridge, L"Guest-local CRT math error handler registration.", reinterpret_cast<ULONGLONG>(&BridgeSetUserMathErr)};
        if (symbol.name == L"_invoke_matherr")
            return {ImportDisposition::NeedsBridge, L"Guest-local CRT math error handler invocation.", reinterpret_cast<ULONGLONG>(&BridgeInvokeMathErr)};
    }
    if ((IsMsvcrt(symbol.library) || ucrt) && symbol.name == L"_endthreadex")
        return {ImportDisposition::NeedsBridge, L"CRT thread exit through the guest boundary.", reinterpret_cast<ULONGLONG>(&BridgeEndThreadEx)};
    if ((IsMsvcrt(symbol.library) || ucrt) && symbol.name == L"_initterm_e")
        return {ImportDisposition::NeedsBridge, L"CRT initializer table with failure propagation.", reinterpret_cast<ULONGLONG>(&BridgeInitTermE)};
    if (ucrt)
    {
        if (symbol.name == L"_beginthreadex") return {ImportDisposition::NeedsBridge, L"CRT guest thread creation.", reinterpret_cast<ULONGLONG>(&BridgeBeginThreadEx)};
        if (symbol.name == L"_initterm") return {ImportDisposition::NeedsBridge, L"CRT initializer table.", reinterpret_cast<ULONGLONG>(&BridgeInitTerm)};
    }
    if (ucrt)
    {
        if (symbol.name == L"rand") return {ImportDisposition::NeedsBridge, L"Guest CRT pseudo-random generator.", reinterpret_cast<ULONGLONG>(&BridgeRand)};
        if (symbol.name == L"srand") return {ImportDisposition::NeedsBridge, L"Guest CRT pseudo-random seed.", reinterpret_cast<ULONGLONG>(&BridgeSrand)};
        if (symbol.name == L"_wtol") return {ImportDisposition::NeedsBridge, L"CRT wide numeric conversion.", reinterpret_cast<ULONGLONG>(&BridgeWtol)};
    }
    if (!IsMsvcrt(symbol.library)) return ResolveCrtDataImport(symbol);

    if (_wcsicmp(symbol.name.c_str(), L"strcmp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStrcmp);
    else if (_wcsicmp(symbol.name.c_str(), L"wcscmp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWcscmp);
    else if (_wcsicmp(symbol.name.c_str(), L"wcsstr") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWcsstr);
    else if (_wcsicmp(symbol.name.c_str(), L"malloc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMalloc);
    else if (_wcsicmp(symbol.name.c_str(), L"free") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFree);
    else if (_wcsicmp(symbol.name.c_str(), L"realloc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRealloc);
    else if (_wcsicmp(symbol.name.c_str(), L"memset") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMemset);
    else if (_wcsicmp(symbol.name.c_str(), L"strlen") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStrlen);
    else if (_wcsicmp(symbol.name.c_str(), L"strchr") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStrchr);
    else if (_wcsicmp(symbol.name.c_str(), L"strstr") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStrstr);
    else if (_wcsicmp(symbol.name.c_str(), L"wcslen") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWcslen);
    else if (_wcsicmp(symbol.name.c_str(), L"memcmp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMemcmp);
    else if (_wcsicmp(symbol.name.c_str(), L"memmove") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMemmove);
    else if (_wcsicmp(symbol.name.c_str(), L"memcpy") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMemcpy);
    else if (_wcsicmp(symbol.name.c_str(), L"_get_osfhandle") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetOsfHandle);
    else if (_wcsicmp(symbol.name.c_str(), L"_isatty") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsatty);
    else if (_wcsicmp(symbol.name.c_str(), L"_beginthreadex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBeginThreadEx);
    else if (_wcsicmp(symbol.name.c_str(), L"__c_specific_handler") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCSpecificHandler);
    else if (_wcsicmp(symbol.name.c_str(), L"__cxxframehandler") == 0) resolution.targetAddress = static_cast<ULONGLONG>(CrtExceptionHelperAddress(L"__CxxFrameHandler3"));
    else if (_wcsicmp(symbol.name.c_str(), L"_cxxthrowexception") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCxxThrowException);
    else if (_wcsicmp(symbol.name.c_str(), L"??1type_info@@ueaa@xz") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTypeInfoDestructor);
    else if (_wcsicmp(symbol.name.c_str(), L"?terminate@@yaxxz") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTerminate);
    else if (_wcsicmp(symbol.name.c_str(), L"_xcptfilter") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeXcptFilter);
    else if (_wcsicmp(symbol.name.c_str(), L"_purecall") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTerminate);
    else if (_wcsicmp(symbol.name.c_str(), L"_initterm") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInitTerm);
    else if (_wcsicmp(symbol.name.c_str(), L"__set_app_type") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetAppType);
    else if (_wcsicmp(symbol.name.c_str(), L"srand") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSrand);
    else if (_wcsicmp(symbol.name.c_str(), L"rand") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRand);
    else if (_wcsicmp(symbol.name.c_str(), L"_wtol") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWtol);
    else if (_wcsicmp(symbol.name.c_str(), L"iswctype") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIswctype);
    else if (_wcsicmp(symbol.name.c_str(), L"wcsrchr") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWcsrchr);
    else if (_wcsicmp(symbol.name.c_str(), L"wcsncmp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWcsncmp);
    else if (_wcsicmp(symbol.name.c_str(), L"_amsg_exit") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAmsgExit);

    if (!resolution.targetAddress) return ResolveCrtDataImport(symbol);

    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"CRT adapter: guest execution boundaries and compatible data operations.";
    }
    return resolution;
}
