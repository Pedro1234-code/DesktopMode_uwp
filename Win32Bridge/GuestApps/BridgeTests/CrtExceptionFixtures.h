#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <eh.h>
#include <exception>
#include <new>
#include <limits>
#include <typeinfo>
#include <cstring>

namespace
{
    struct ObservedThrowMetadata { void* info = nullptr; uintptr_t base = 0; };
    int ObserveThrowFilter(EXCEPTION_POINTERS* exception, ObservedThrowMetadata* metadata)
    {
        const auto record = exception->ExceptionRecord;
        if (record->ExceptionCode == 0xE06D7363u && record->NumberParameters == 4)
        {
            metadata->info = reinterpret_cast<void*>(record->ExceptionInformation[2]);
            metadata->base = record->ExceptionInformation[3];
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void ObserveNativeThrow(Win32Bridge::Bridge::CrtExecutionCallback callback, void* context, ObservedThrowMetadata* metadata)
    {
        __try { callback(context); }
        __except(ObserveThrowFilter(GetExceptionInformation(), metadata)) { }
    }
    uintptr_t __cdecl NativeFixtureImageBase(const void* address)
    {
        void* base = nullptr;
        ::RtlPcToFileHeader(const_cast<void*>(address), &base);
        return reinterpret_cast<uintptr_t>(base);
    }
    int g_exceptionObjectDestructions = 0;
    struct ExceptionFixtureBase
    {
        int value;
        explicit ExceptionFixtureBase(int input) : value(input) { }
        virtual ~ExceptionFixtureBase() = default;
    };
    struct ExceptionFixtureDerived final : ExceptionFixtureBase
    {
        explicit ExceptionFixtureDerived(int input) : ExceptionFixtureBase(input) { }
        ~ExceptionFixtureDerived() override { ++g_exceptionObjectDestructions; }
    };
    struct ExceptionUnwindMarker
    {
        int* count;
        ~ExceptionUnwindMarker() { ++*count; }
    };
    void __cdecl ThrowFixtureInteger(void*) { throw 42; }
    void __cdecl ThrowFixtureDerived(void*) { throw ExceptionFixtureDerived(91); }
    void __cdecl NativeTerminateFixture(void*) { ::terminate(); }
    void __cdecl ShimTerminateFixture(void*) { Win32Bridge::Bridge::TerminateCrtExecution(); }
    __declspec(noinline) void ThrowForNoexceptFixture() { throw 55; }
    void __cdecl NoexceptFixture(void*) noexcept { ThrowForNoexceptFixture(); }
    int __cdecl SumExceptionFixture(int first, int second) { return first + second; }
    int g_guestTerminateCalls = 0;
    void __cdecl ReturningGuestTerminateFixture() { ++g_guestTerminateCalls; }
    void __cdecl ThrowingGuestTerminateFixture() { ++g_guestTerminateCalls; throw 12; }
    void __cdecl InstallReturningTerminateFixture(void*)
    {
        Win32Bridge::Bridge::SetCrtTerminateHandler(&ReturningGuestTerminateFixture);
        ::terminate();
    }
    void __cdecl InstallThrowingTerminateFixture(void*)
    {
        Win32Bridge::Bridge::SetCrtTerminateHandler(&ThrowingGuestTerminateFixture);
        ::terminate();
    }
    void __cdecl HostTerminateFixture() { ++g_guestTerminateCalls; }
    void __cdecl NestedTerminateFixture(void* context)
    {
        const bool result = Win32Bridge::Bridge::InvokeCrtExecution(&ShimTerminateFixture, nullptr);
        *static_cast<bool*>(context) = !result;
    }

    bool ValidateCrtExceptionImageFixtures()
    {
        using namespace Win32Bridge::Bridge;
        const auto base = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12340000));
        if (RegisterCrtGuestImage(nullptr, 0x1000) || RegisterCrtGuestImage(base, 0)) return false;
        if (!RegisterCrtGuestImage(base, 0x2000)) return false;
        const bool located = CrtGuestImageBase(base) == 0x12340000 &&
            CrtGuestImageBase(reinterpret_cast<void*>(static_cast<uintptr_t>(0x12341fff))) == 0x12340000 &&
            !CrtGuestImageBase(reinterpret_cast<void*>(static_cast<uintptr_t>(0x12342000))) &&
            !RegisterCrtGuestImage(reinterpret_cast<void*>(static_cast<uintptr_t>(0x12341000)), 0x2000) &&
            !RegisterCrtGuestImage(base, 0x3000);
        UnregisterCrtGuestImage(base);
        return located && !CrtGuestImageBase(base);
    }
    bool ValidateCrtExceptionDispatchFixtures()
    {
        using namespace Win32Bridge::Bridge;
        ObservedThrowMetadata integer, derived;
        try { ObserveNativeThrow(&ThrowFixtureInteger, nullptr, &integer); }
        catch (int value) { if (value != 42) return false; }
        try { ObserveNativeThrow(&ThrowFixtureDerived, nullptr, &derived); }
        catch (const ExceptionFixtureBase& value) { if (value.value != 91) return false; }
        if (!integer.info || !integer.base || !derived.info || !derived.base) return false;
        int number = 77, unwound = 0; bool caught = false;
        try
        {
            ExceptionUnwindMarker marker{ &unwound };
            RaiseCrtException(&number, integer.info, &NativeFixtureImageBase);
        }
        catch (int value) { caught = value == 77; }
        if (!caught || unwound != 1) return false;
        alignas(ExceptionFixtureDerived) unsigned char storage[sizeof(ExceptionFixtureDerived)];
        auto object = new (storage) ExceptionFixtureDerived(103);
        g_exceptionObjectDestructions = 0; caught = false;
        try { RaiseCrtException(object, derived.info, &NativeFixtureImageBase); }
        catch (const ExceptionFixtureBase& value) { caught = value.value == 103; }
        if (!caught || g_exceptionObjectDestructions != 1) return false;
        try
        {
            try { RaiseCrtException(&number, integer.info, &NativeFixtureImageBase); }
            catch (int) { RaiseCrtException(nullptr, nullptr); }
        }
        catch (int value) { caught = value == 77; }
        return caught && CrtExceptionHelperAddress(L"__CxxFrameHandler3") && CrtExceptionHelperAddress(L"__CxxFrameHandler4") &&
            CrtExceptionHelperAddress(L"__CxxFrameHandler") == CrtExceptionHelperAddress(L"__CxxFrameHandler3") &&
            !CrtExceptionHelperAddress(L"_a_function_that_does_not_exist");
    }
    bool ValidateCrtExceptionThreadStateFixtures()
    {
        using namespace Win32Bridge::Bridge;
        CrtThreadExceptionState first, second;
        auto previous = SetCrtThreadExceptionState(&first);
        g_guestTerminateCalls = 0;
        bool valid = !InvokeCrtExecution(&InstallReturningTerminateFixture, nullptr) && g_guestTerminateCalls == 1 &&
            GetCrtTerminateHandler() == &ReturningGuestTerminateFixture;
        SetCrtThreadExceptionState(&second);
        valid = valid && GetCrtTerminateHandler() != &ReturningGuestTerminateFixture &&
            !InvokeCrtExecution(&NativeTerminateFixture, nullptr) && g_guestTerminateCalls == 1;
        SetCrtThreadExceptionState(&first);
        valid = valid && GetCrtTerminateHandler() == &ReturningGuestTerminateFixture &&
            !InvokeCrtExecution(&NativeTerminateFixture, nullptr) && g_guestTerminateCalls == 2;
        SetCrtThreadExceptionState(previous);
        return valid;
    }
    bool ValidateCrtExceptionBoundaryFixtures()
    {
        using namespace Win32Bridge::Bridge;
        using CurrentException = void**(__cdecl*)();
        using ProcessingThrow = int*(__cdecl*)();
        const auto current = reinterpret_cast<CurrentException>(CrtExceptionHelperAddress(L"__current_exception"));
        const auto context = reinterpret_cast<CurrentException>(CrtExceptionHelperAddress(L"__current_exception_context"));
        const auto processing = reinterpret_cast<ProcessingThrow>(CrtExceptionHelperAddress(L"__processing_throw"));
        if (!current || !context || !processing) return false;
        const auto previousTerminate = ::set_terminate(&HostTerminateFixture);
        const auto savedException = *current(), savedContext = *context(); const int savedProcessing = *processing();
        int sum = 0; bool nested = false;
        bool valid = InvokeCrtFunction(&sum, &SumExceptionFixture, 7, 8) && sum == 15 &&
            !InvokeCrtExecution(&NativeTerminateFixture, nullptr) &&
            !InvokeCrtExecution(&ThrowFixtureInteger, nullptr) &&
            !InvokeCrtExecution(&NoexceptFixture, nullptr) &&
            InvokeCrtExecution(&NestedTerminateFixture, &nested) && nested;
        g_guestTerminateCalls = 0;
        valid = valid && !InvokeCrtExecution(&InstallReturningTerminateFixture, nullptr) && g_guestTerminateCalls == 1;
        valid = valid && !InvokeCrtExecution(&InstallThrowingTerminateFixture, nullptr) && g_guestTerminateCalls == 2;
        valid = valid && ::_get_terminate() == &HostTerminateFixture && *current() == savedException &&
            *context() == savedContext && *processing() == savedProcessing;
        ::set_terminate(previousTerminate);
        if (!valid) return false;
        // A nested guest termination must not damage an already active host
        // catch or its ability to rethrow the original exception afterwards.
        bool rethrown = false;
        try
        {
            try { throw 93; }
            catch (int)
            {
                const auto active = *current(); const int activeCount = *processing();
                if (InvokeCrtExecution(&NativeTerminateFixture, nullptr) || *current() != active || *processing() != activeCount) return false;
                throw;
            }
        }
        catch (int value) { rethrown = value == 93; }
        return rethrown;
    }
}
