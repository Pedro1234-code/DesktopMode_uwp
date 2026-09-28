#include "pch.h"
#include "Bridge/MsvcrtShims.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
    int g_commode = 0;
    int g_fmode = 0;
    char g_commandLine[] = "";
    char* g_acmdln = g_commandLine;
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
    intptr_t __cdecl BridgeGetOsfHandle(int) { return -1; }
    int __cdecl BridgeIsatty(int) { return 0; }
    FILE* __cdecl BridgeIob() { return nullptr; }
    uintptr_t __cdecl BridgeBeginThreadEx(void*, unsigned, unsigned(__stdcall*)(void*), void*, unsigned, unsigned* threadId) { if (threadId) *threadId = 0; return 0; }
    EXCEPTION_DISPOSITION __cdecl BridgeCSpecificHandler(PEXCEPTION_RECORD, PVOID, PCONTEXT, PDISPATCHER_CONTEXT) { return ExceptionContinueSearch; }
    EXCEPTION_DISPOSITION __cdecl BridgeCxxFrameHandler(PEXCEPTION_RECORD, PVOID, PCONTEXT, PDISPATCHER_CONTEXT) { return ExceptionContinueSearch; }
    void __cdecl BridgeCxxThrowException(void*, void*)
    {
        // Returning from _CxxThrowException is invalid: callers assume the
        // stack has been unwound. Until the guest CRT's full catch/type-info
        // machinery is implemented, raise a non-continuable bridge exception
        // so the runtime's top-level SEH boundary can stop this one guest
        // cleanly instead of continuing into a DebugBreak or corrupted state.
        Win32Bridge::Bridge::RuntimeDiagnostics::Record(L"CRT: guest requested C++ exception; controlled guest shutdown.");
        RaiseException(GuestCxxExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    void __cdecl BridgeTypeInfoDestructor() { }
    void __cdecl BridgeTerminate()
    {
        Win32Bridge::Bridge::RuntimeDiagnostics::Record(L"CRT: guest called terminate/purecall; controlled guest shutdown.");
        RaiseException(GuestCxxExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    void __cdecl BridgeExit(int) { }
    void __cdecl BridgeCExit() { }
    void* __cdecl BridgeDllOnExit(void*, void**, void**) { return nullptr; }
    void* __cdecl BridgeOnExit(void*) { return nullptr; }
    int __cdecl BridgeXcptFilter(unsigned long, PEXCEPTION_POINTERS) { return EXCEPTION_CONTINUE_SEARCH; }
    int __cdecl BridgeGetMainArgs(int* argumentCount, char*** argumentValues, char*** environment, int, int*)
    {
        if (argumentCount) *argumentCount = 0;
        if (argumentValues) *argumentValues = nullptr;
        if (environment) *environment = nullptr;
        return 0;
    }
    void __cdecl BridgeInitTerm(void(__cdecl** first)(), void(__cdecl** last)())
    {
        if (!first || !last) return;
        for (auto current = first; current != last; ++current) if (*current) (*current)();
    }
    void* __cdecl BridgeSetUserMathErr(void*) { return nullptr; }
    void __cdecl BridgeSetAppType(int) { }
    unsigned int g_randomState = 1;
    void __cdecl BridgeSrand(unsigned int seed) { g_randomState = seed ? seed : 1; }
    int __cdecl BridgeRand() { g_randomState = g_randomState * 1103515245u + 12345u; return static_cast<int>((g_randomState >> 16) & 0x7fff); }
}

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveMsvcrtImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || !IsMsvcrt(symbol.library)) return resolution;

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
    else if (_wcsicmp(symbol.name.c_str(), L"_iob") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIob);
    else if (_wcsicmp(symbol.name.c_str(), L"_beginthreadex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBeginThreadEx);
    else if (_wcsicmp(symbol.name.c_str(), L"__c_specific_handler") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCSpecificHandler);
    else if (_wcsicmp(symbol.name.c_str(), L"__cxxframehandler") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCxxFrameHandler);
    else if (_wcsicmp(symbol.name.c_str(), L"_cxxthrowexception") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCxxThrowException);
    else if (_wcsicmp(symbol.name.c_str(), L"??1type_info@@ueaa@xz") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTypeInfoDestructor);
    else if (_wcsicmp(symbol.name.c_str(), L"?terminate@@yaxxz") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTerminate);
    else if (_wcsicmp(symbol.name.c_str(), L"__dllonexit") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDllOnExit);
    else if (_wcsicmp(symbol.name.c_str(), L"_onexit") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOnExit);
    else if (_wcsicmp(symbol.name.c_str(), L"_xcptfilter") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeXcptFilter);
    else if (_wcsicmp(symbol.name.c_str(), L"_exit") == 0 || _wcsicmp(symbol.name.c_str(), L"exit") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExit);
    else if (_wcsicmp(symbol.name.c_str(), L"_cexit") == 0 || _wcsicmp(symbol.name.c_str(), L"_c_exit") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCExit);
    else if (_wcsicmp(symbol.name.c_str(), L"_purecall") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTerminate);
    else if (_wcsicmp(symbol.name.c_str(), L"__getmainargs") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMainArgs);
    else if (_wcsicmp(symbol.name.c_str(), L"_initterm") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInitTerm);
    else if (_wcsicmp(symbol.name.c_str(), L"__setusermatherr") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetUserMathErr);
    else if (_wcsicmp(symbol.name.c_str(), L"__set_app_type") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetAppType);
    else if (_wcsicmp(symbol.name.c_str(), L"srand") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSrand);
    else if (_wcsicmp(symbol.name.c_str(), L"rand") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRand);
    else if (_wcsicmp(symbol.name.c_str(), L"_commode") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&g_commode);
    else if (_wcsicmp(symbol.name.c_str(), L"_fmode") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&g_fmode);
    else if (_wcsicmp(symbol.name.c_str(), L"_acmdln") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&g_acmdln);

    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"CRT adapter: basic memory, strings and startup globals are guest-local.";
    }
    return resolution;
}
