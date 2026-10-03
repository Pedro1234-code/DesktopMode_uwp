#include "Bridge\\GuestRuntime.h"
#include "Bridge/ActivationContext.h"
#include "Bridge\\GuestKernel.h"
#include "Bridge\\GuestModule.h"
#include "Bridge/GuestRegistry.h"
#include "Bridge/GuestResources.h"
#include "Bridge\\GuestWindow.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge/Win32Shims.h"

#include <atomic>
#include <cwchar>
#include <roapi.h>
#include <system_error>
#include <thread>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local GuestRuntime* g_currentGuestRuntime = nullptr;
    std::atomic<unsigned> g_activeGuestRuntimeScopes{ 0 };
    std::atomic<DWORD> g_nextGuestProcessId{ 1000 };

    struct GuestExceptionDetails final
    {
        DWORD code = ERROR_SUCCESS;
        ULONG_PTR address = 0;
        ULONG_PTR instructionPointer = 0;
        ULONG_PTR stackPointer = 0;
        ULONG_PTR accessOperation = static_cast<ULONG_PTR>(-1);
        ULONG_PTR faultAddress = 0;
    };

    struct CurrentGuestRuntimeScope final
    {
        explicit CurrentGuestRuntimeScope(GuestRuntime* runtime)
            : previous(g_currentGuestRuntime)
        {
            g_currentGuestRuntime = runtime;
        }
        ~CurrentGuestRuntimeScope() { g_currentGuestRuntime = previous; }
        GuestRuntime* previous;
    };

    struct GuestResourceHandleScope final
    {
        GuestResourceHandleScope()
        {
            if (g_activeGuestRuntimeScopes.fetch_add(1) == 0)
                ResetGuestResourceHandles();
        }
        ~GuestResourceHandleScope()
        {
            if (g_activeGuestRuntimeScopes.fetch_sub(1) == 1)
                ResetGuestResourceHandles();
        }
    };

    void SetError(std::wstring* error, const std::wstring& message)
    {
        if (error)
        {
            *error = message;
        }
    }

    int CaptureGuestException(EXCEPTION_POINTERS* information, GuestExceptionDetails* details)
    {
        if (!details) return EXCEPTION_EXECUTE_HANDLER;
        *details = GuestExceptionDetails{};
        if (!information) return EXCEPTION_EXECUTE_HANDLER;

        if (information->ExceptionRecord)
        {
            const EXCEPTION_RECORD* record = information->ExceptionRecord;
            details->code = record->ExceptionCode;
            details->address = reinterpret_cast<ULONG_PTR>(record->ExceptionAddress);
            if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                 record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
                record->NumberParameters >= 2)
            {
                details->accessOperation = record->ExceptionInformation[0];
                details->faultAddress = record->ExceptionInformation[1];
            }
        }
#if defined(_M_X64)
        if (information->ContextRecord)
        {
            details->instructionPointer = static_cast<ULONG_PTR>(information->ContextRecord->Rip);
            details->stackPointer = static_cast<ULONG_PTR>(information->ContextRecord->Rsp);
        }
#endif
        return EXCEPTION_EXECUTE_HANDLER;
    }

    std::wstring HexAddress(ULONG_PTR value)
    {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"0x%llX", static_cast<unsigned long long>(value));
        return buffer;
    }

    std::wstring DescribeGuestException(
        const GuestExceptionDetails& details,
        const BYTE* mainImageBase,
        size_t mainImageSize,
        const std::wstring& mainImageName,
        const GuestModuleLoader* modules)
    {
        std::wstring location;
        const ULONG_PTR imageBase = reinterpret_cast<ULONG_PTR>(mainImageBase);
        if (mainImageBase && details.address >= imageBase &&
            details.address - imageBase < mainImageSize)
        {
            location = mainImageName.empty() ? L"main image" : mainImageName;
            location += L"+" + HexAddress(details.address - imageBase);
        }
        else if (modules)
        {
            modules->DescribeAddress(details.address, &location);
        }

        std::wstring result = L"code " + std::to_wstring(details.code) +
            L" at " + HexAddress(details.address);
        if (!location.empty()) result += L" (" + location + L")";
        if (details.accessOperation != static_cast<ULONG_PTR>(-1))
        {
            const wchar_t* operation = details.accessOperation == 0 ? L"read" :
                details.accessOperation == 1 ? L"write" :
                details.accessOperation == 8 ? L"execute" : L"access";
            result += L", " + std::wstring(operation) + L" " + HexAddress(details.faultAddress);
        }
        if (details.instructionPointer && details.instructionPointer != details.address)
            result += L", RIP " + HexAddress(details.instructionPointer);
        if (details.stackPointer)
            result += L", RSP " + HexAddress(details.stackPointer);
        return result;
    }

    // These are intentionally small SEH boundaries. A guest PE shares our process,
    // and an SEH failure in its message loop would otherwise terminate the
    // CoreShell host before RuntimeSession can persist diagnostics.
    bool InvokeGuestEntryPoint(
        int(WINAPI* entryPoint)(),
        int* exitCode,
        GuestExceptionDetails* exception)
    {
        if (exception) *exception = GuestExceptionDetails{};

        __try
        {
            *exitCode = entryPoint();
            return true;
        }
        __except (CaptureGuestException(GetExceptionInformation(), exception))
        {
            return false;
        }
    }

    bool InvokeGuestThreadEntry(
        LPTHREAD_START_ROUTINE startAddress,
        LPVOID parameter,
        DWORD* exitCode,
        GuestExceptionDetails* exception)
    {
        if (exception) *exception = GuestExceptionDetails{};
        __try
        {
            *exitCode = startAddress(parameter);
            return true;
        }
        __except (CaptureGuestException(GetExceptionInformation(), exception))
        {
            return false;
        }
    }
}

bool GuestRuntime::SetResourceSatellite(
    const BYTE* fileBytes,
    size_t fileSize,
    std::wstring* error)
{
    m_resourceSatellite = MappedPeImage{};
    if (!fileBytes || fileSize == 0) return true;
    if (!PeMapper::Materialize(fileBytes, fileSize, &m_resourceSatellite, error))
    {
        RuntimeDiagnostics::Record(L"MUI: companion image could not be materialized.");
        return false;
    }
    RuntimeDiagnostics::Record(L"MUI: attached resource satellite (" +
        std::to_wstring(fileSize) + L" bytes).");
    return true;
}

bool GuestRuntime::Prepare(const BYTE* fileBytes, size_t fileSize, const ImportResolver& resolver, std::wstring* error)
{
    RuntimeDiagnostics::Record(L"PREPARE: inspecting main PE (" + std::to_wstring(fileSize) + L" bytes).");
    m_ready = false;
    m_runtime.Release();
    m_metadata = PeImageInfo{};
    m_mapped = MappedPeImage{};
    m_bindings = BindingReport{};
    m_kernel.reset();
    m_modules.reset();
    m_registry.reset();
    m_resolver = resolver;

    if (!PeImage::Inspect(fileBytes, fileSize, &m_metadata))
    {
        SetError(error, m_metadata.error);
        RuntimeDiagnostics::Record(L"PREPARE FAILED: PE inspection: " + m_metadata.error);
        return false;
    }
    if (!PeMapper::Materialize(fileBytes, fileSize, &m_mapped, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: PE materialization.");
        return false;
    }
    if (!RuntimeImage::Reserve(m_mapped.bytes.size(), m_mapped.preferredImageBase, &m_runtime, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: executable image reservation.");
        return false;
    }
    if (!PeMapper::ApplyBaseRelocations(&m_mapped, reinterpret_cast<ULONGLONG>(m_runtime.Base()), error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: base relocations.");
        return false;
    }
    if (!ImportBinder::Bind(&m_mapped, m_metadata, resolver, &m_bindings, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: import binding.");
        return false;
    }
    if (m_bindings.unresolved != 0)
    {
        SetError(error, std::to_wstring(m_bindings.unresolved) +
            L" guest imports are still unresolved; execution was not attempted.");
        RuntimeDiagnostics::Record(L"PREPARE FAILED: " + std::to_wstring(m_bindings.unresolved) + L" unresolved main imports.");
        return false;
    }
    if (!m_runtime.CopyFrom(m_mapped, error) || !m_runtime.FinalizeProtections(m_mapped, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: image copy or final page protections.");
        return false;
    }

    m_ready = true;
    RuntimeDiagnostics::Record(
        L"PREPARE OK: main image reserved; " + std::to_wstring(m_bindings.bound) +
        L" imports bound; entry RVA " + std::to_wstring(m_mapped.entryPointRva) + L".");
    return true;
}

bool GuestRuntime::Run(int* exitCode, std::wstring* error)
{
    if (!m_ready || !exitCode)
    {
        SetError(error, L"The guest has not been fully prepared for execution.");
        RuntimeDiagnostics::Record(L"RUN FAILED: called before a successful prepare.");
        return false;
    }

    // Kernel objects are per invocation, just like the current guest storage
    // and window scopes.  They must not survive a later run of the same image.
    m_kernel = std::make_shared<GuestKernelContext>();
    m_modules = std::make_shared<GuestModuleLoader>(
        m_storage,
        m_resolver,
        m_moduleSourceFolder.Get());
    if (!m_registry)
    {
        m_registry = std::make_shared<GuestRegistryContext>(m_storage);
    }
    GuestKernelScope kernelScope(m_kernel.get());
    GuestModuleScope moduleScope(m_modules.get());
    GuestRegistryScope registryScope(m_registry.get());
    GuestResourceHandleScope resourceHandleScope;
    GuestResourceScope resourceScope(
        m_runtime.Base(), m_runtime.Size(),
        m_resourceSatellite.bytes.empty() ? nullptr : m_resourceSatellite.bytes.data(),
        m_resourceSatellite.bytes.size());
    GuestStorageScope storageScope(m_storage.get());
    GuestActivationContextScope activationContextScope;
    GuestWindowScope windowScope(m_windows.get(), !m_sharedWindowManager);
    CurrentGuestRuntimeScope runtimeScope(this);
    GuestCommandLineScope commandLineScope(
        m_commandLine.empty() && m_storage
            ? L"\"" + m_storage->ModulePath() + L"\""
            : m_commandLine,
        m_processId,
        m_mainThreadId);
    if (m_storage && !m_storage->EnsureLayout(error))
    {
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        RuntimeDiagnostics::Record(L"RUN FAILED: virtual drive layout could not be ensured.");
        return false;
    }
    DWORD registryError = ERROR_SUCCESS;
    if (!m_registry->Initialize(&registryError))
    {
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, L"Could not initialize the guest registry (" +
            std::to_wstring(registryError) + L").");
        RuntimeDiagnostics::Record(L"RUN FAILED: guest registry initialization error " +
            std::to_wstring(registryError) + L".");
        return false;
    }
    if (m_storage)
    {
        DWORD storageError = ERROR_SUCCESS;
        if (!m_storage->ResetCurrentDirectory(&storageError))
        {
            m_kernel->CloseAll();
            m_modules->ReleaseAll();
            SetError(error, L"Could not reset the guest current directory (" + std::to_wstring(storageError) + L").");
            RuntimeDiagnostics::Record(L"RUN FAILED: guest current-directory reset error " + std::to_wstring(storageError) + L".");
            return false;
        }
        if (!m_initialCurrentDirectory.empty() &&
            !m_storage->SetCurrentDirectory(m_initialCurrentDirectory.c_str(), &storageError))
        {
            m_kernel->CloseAll();
            m_modules->ReleaseAll();
            SetError(error, L"Could not set the child current directory (" +
                std::to_wstring(storageError) + L").");
            RuntimeDiagnostics::Record(L"RUN FAILED: child current-directory error " +
                std::to_wstring(storageError) + L".");
            return false;
        }
    }

    using GuestEntryPoint = int(WINAPI*)();
    const auto entryPoint = reinterpret_cast<GuestEntryPoint>(m_runtime.Base() + m_mapped.entryPointRva);
    try
    {
        RuntimeDiagnostics::Record(L"RUN: entering guest entry point.");
        GuestExceptionDetails guestException;
        if (!InvokeGuestEntryPoint(entryPoint, exitCode, &guestException))
        {
            const std::wstring exceptionDescription = DescribeGuestException(
                guestException,
                m_runtime.Base(),
                m_runtime.Size(),
                m_storage ? m_storage->ModulePath() : std::wstring(),
                m_modules.get());
            if (m_storage)
            {
                m_storage->CloseAll();
            }
            m_kernel->CloseAll();
            m_modules->ReleaseAll();
            SetError(error, L"The guest raised a structured exception: " +
                exceptionDescription + L".");
            RuntimeDiagnostics::Record(L"RUN FAILED: guest structured exception " +
                exceptionDescription + L".");
            return false;
        }
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        RuntimeDiagnostics::Record(L"RUN: guest returned exit code " + std::to_wstring(*exitCode) + L".");
        return true;
    }
    catch (Platform::Exception^ exception)
    {
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, L"The guest raised an unhandled WinRT exception.");
        RuntimeDiagnostics::Record(L"RUN FAILED: guest raised a WinRT exception; HRESULT " +
            std::to_wstring(static_cast<unsigned long>(exception->HResult)) + L".");
        return false;
    }
    catch (...)
    {
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, L"The guest raised an unhandled C++ exception.");
        RuntimeDiagnostics::Record(L"RUN FAILED: guest raised an unhandled C++ exception.");
        return false;
    }
}

bool GuestRuntime::LaunchChildProcess(
    LPCWSTR executableName,
    LPCWSTR commandLine,
    LPCWSTR currentDirectory,
    LPPROCESS_INFORMATION processInformation,
    DWORD* win32Error)
{
    if (!processInformation || !executableName || !*executableName ||
        !m_modules || !m_kernel || !m_storage || !m_windows)
    {
        if (win32Error) *win32Error = ERROR_INVALID_PARAMETER;
        return false;
    }
    ZeroMemory(processInformation, sizeof(*processInformation));

    std::vector<BYTE> bytes;
    std::wstring logicalPath;
    DWORD errorCode = ERROR_SUCCESS;
    if (!m_modules->ReadExecutableBytes(
        executableName, &bytes, &logicalPath, &errorCode))
    {
        if (win32Error) *win32Error = errorCode;
        RuntimeDiagnostics::Record(
            L"PROCESS FAILED: executable was not found: " + std::wstring(executableName) + L".");
        return false;
    }

    std::shared_ptr<GuestStorageContext> childStorage;
    std::shared_ptr<GuestRuntime> child;
    try
    {
        childStorage = std::make_shared<GuestStorageContext>(
            m_storage->LocalFolder(), logicalPath);
        child = std::make_shared<GuestRuntime>();
    }
    catch (const std::bad_alloc&)
    {
        if (win32Error) *win32Error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    child->SetStorageContext(childStorage);
    // The registry is machine/user state, not process-private state. Sharing
    // one synchronized logical context also prevents two cooperative guests
    // from overwriting each other's persisted hive snapshots.
    child->SetRegistryContext(m_registry);
    child->SetWindowManager(m_windows);
    child->SetModuleSourceFolder(m_modules->ModuleSourceFolder());
    child->SetCommandLine(commandLine ? commandLine : executableName);
    child->SetInitialCurrentDirectory(currentDirectory ? currentDirectory : L"");
    child->SetSharedWindowManager(true);

    std::wstring prepareError;
    if (!child->Prepare(bytes.data(), bytes.size(), m_resolver, &prepareError))
    {
        if (win32Error) *win32Error = ERROR_BAD_EXE_FORMAT;
        RuntimeDiagnostics::Record(
            L"PROCESS FAILED: child preparation failed for " + logicalPath + L": " + prepareError);
        return false;
    }

    const std::shared_ptr<GuestKernelContext> completionKernel = m_kernel;
    HANDLE processHandle = completionKernel->CreateEvent(true, false, nullptr, &errorCode);
    if (!processHandle)
    {
        if (win32Error) *win32Error = errorCode;
        return false;
    }
    HANDLE threadHandle = completionKernel->CreateEvent(true, false, nullptr, &errorCode);
    if (!threadHandle)
    {
        completionKernel->CloseHandle(processHandle, nullptr);
        if (win32Error) *win32Error = errorCode;
        return false;
    }

    const DWORD processId = g_nextGuestProcessId.fetch_add(1);
    const DWORD threadId = g_nextGuestProcessId.fetch_add(1);
    child->SetProcessId(processId);
    child->SetMainThreadId(threadId);
    processInformation->hProcess = processHandle;
    processInformation->hThread = threadHandle;
    processInformation->dwProcessId = processId;
    processInformation->dwThreadId = threadId;

    RuntimeDiagnostics::Record(
        L"PROCESS: entering a cooperative child guest for " + logicalPath + L".");
    const HWND previousForeground = m_windows->GetGuestForegroundWindow();
    DWORD activationError = ERROR_SUCCESS;
    const HWND previousFocus = m_windows->GetGuestFocus(&activationError);
    int exitCode = -1;
    std::wstring childError;
    const bool completed = child->Run(&exitCode, &childError);
    // A process owns its USER objects. Purge the child's window/DC/message
    // state before releasing its mapped image, then reactivate the caller.
    m_windows->DestroyGuestWindowsForProcess(
        processId, previousForeground, previousFocus);
    DWORD ignored = ERROR_SUCCESS;
    completionKernel->SetEvent(threadHandle, &ignored);
    completionKernel->SetEvent(processHandle, &ignored);
    RuntimeDiagnostics::Record(
        completed
            ? L"PROCESS: child returned from " + logicalPath +
                L" with exit code " + std::to_wstring(exitCode) + L"."
            : L"PROCESS FAILED: child stopped in " + logicalPath + L": " + childError);
    if (win32Error) *win32Error = ERROR_SUCCESS;
    return true;
}

HANDLE GuestRuntime::LaunchThread(
    SIZE_T,
    LPTHREAD_START_ROUTINE startAddress,
    LPVOID parameter,
    DWORD creationFlags,
    LPDWORD threadId,
    DWORD* win32Error)
{
    if (threadId) *threadId = 0;
    if (!startAddress || !m_kernel || !m_modules || !m_registry ||
        !m_storage || !m_windows)
    {
        if (win32Error) *win32Error = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    if ((creationFlags & CREATE_SUSPENDED) != 0 ||
        (creationFlags & ~static_cast<DWORD>(STACK_SIZE_PARAM_IS_A_RESERVATION)) != 0)
    {
        if (win32Error) *win32Error = ERROR_NOT_SUPPORTED;
        return nullptr;
    }

    DWORD errorCode = ERROR_SUCCESS;
    HANDLE completion = m_kernel->CreateEvent(true, false, nullptr, &errorCode);
    if (!completion)
    {
        if (win32Error) *win32Error = errorCode;
        return nullptr;
    }

    const DWORD assignedThreadId = g_nextGuestProcessId.fetch_add(1);
    const std::shared_ptr<GuestKernelContext> kernel = m_kernel;
    const std::shared_ptr<GuestModuleLoader> modules = m_modules;
    const std::shared_ptr<GuestRegistryContext> registry = m_registry;
    const std::shared_ptr<GuestStorageContext> storage = m_storage;
    const std::shared_ptr<GuestWindowManager> windows = m_windows;
    const BYTE* imageBase = m_runtime.Base();
    const size_t imageSize = m_runtime.Size();
    const BYTE* resourceSatelliteBase = m_resourceSatellite.bytes.empty()
        ? nullptr : m_resourceSatellite.bytes.data();
    const size_t resourceSatelliteSize = m_resourceSatellite.bytes.size();
    const std::wstring commandLine = m_commandLine.empty() && m_storage
        ? L"\"" + m_storage->ModulePath() + L"\""
        : m_commandLine;
    const DWORD processId = m_processId;
    GuestRuntime* const runtime = this;

    try
    {
        std::thread([
            kernel, modules, registry, storage, windows,
            imageBase, imageSize, resourceSatelliteBase, resourceSatelliteSize,
            commandLine, processId, assignedThreadId, runtime,
            completion, startAddress, parameter]()
        {
            const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
            GuestKernelScope kernelScope(kernel.get());
            GuestModuleScope moduleScope(modules.get());
            GuestRegistryScope registryScope(registry.get());
            GuestResourceScope resourceScope(
                imageBase, imageSize, resourceSatelliteBase, resourceSatelliteSize);
            GuestStorageScope storageScope(storage.get());
            GuestActivationContextScope activationContextScope;
            GuestWindowScope windowScope(windows.get(), false);
            CurrentGuestRuntimeScope runtimeScope(runtime);
            GuestCommandLineScope commandLineScope(commandLine, processId, assignedThreadId);

            DWORD exitCode = 0;
            GuestExceptionDetails exception;
            if (!InvokeGuestThreadEntry(
                startAddress, parameter, &exitCode, &exception))
            {
                const std::wstring exceptionDescription = DescribeGuestException(
                    exception,
                    imageBase,
                    imageSize,
                    storage ? storage->ModulePath() : std::wstring(),
                    modules.get());
                RuntimeDiagnostics::Record(
                    L"THREAD FAILED: guest structured exception " +
                    exceptionDescription + L".");
            }
            DWORD ignored = ERROR_SUCCESS;
            kernel->SetEvent(completion, &ignored);
            if (SUCCEEDED(apartment)) RoUninitialize();
        }).detach();
    }
    catch (const std::system_error&)
    {
        m_kernel->CloseHandle(completion, nullptr);
        if (win32Error) *win32Error = ERROR_NOT_ENOUGH_MEMORY;
        return nullptr;
    }

    if (threadId) *threadId = assignedThreadId;
    if (win32Error) *win32Error = ERROR_SUCCESS;
    RuntimeDiagnostics::Record(
        L"THREAD: launched guest thread " + std::to_wstring(assignedThreadId) + L".");
    return completion;
}

GuestRuntime* Win32Bridge::Bridge::CurrentGuestRuntime()
{
    return g_currentGuestRuntime;
}
