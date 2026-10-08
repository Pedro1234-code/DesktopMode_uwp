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
    constexpr DWORD GuestExitException = 0xE0424242u;
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
        bool requestedExit = false;
        bool threadExit = false;
        DWORD requestedExitCode = 0;
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
            if (record->ExceptionCode == GuestExitException && record->NumberParameters >= 1)
            {
                details->requestedExit = true;
                details->requestedExitCode = static_cast<DWORD>(record->ExceptionInformation[0]);
                details->threadExit = record->NumberParameters >= 2 &&
                    record->ExceptionInformation[1] != 0;
            }
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
    // Keep a writable materialization until Run establishes guest scopes and
    // the DLL loader. Imports that are not bridge APIs are resolved there from
    // actual guest DLLs, including ordinal and delay-import entries.
    if (!m_runtime.CopyFrom(m_mapped, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: initial image copy.");
        return false;
    }

    m_ready = true;
    RuntimeDiagnostics::Record(
        L"PREPARE OK: main image reserved; " + std::to_wstring(m_bindings.bound) +
        L" bridge imports bound, " + std::to_wstring(m_bindings.unresolved) +
        L" imports deferred to the guest DLL loader; entry RVA " +
        std::to_wstring(m_mapped.entryPointRva) + L".");
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

    const ImportResolver fullResolver = [this](const ImportedSymbol& symbol)
    {
        return m_modules->ResolveImport(symbol);
    };
    BindingReport finalBindings;
    std::wstring bindingError;
    if (!ImportBinder::Bind(
            &m_mapped, m_metadata, fullResolver, &finalBindings, &bindingError) ||
        finalBindings.unresolved != 0)
    {
        if (m_storage) m_storage->CloseAll();
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        m_bindings = std::move(finalBindings);
        SetError(error, bindingError.empty()
            ? std::to_wstring(m_bindings.unresolved) +
                L" guest imports remain unresolved after DLL loading."
            : bindingError);
        RuntimeDiagnostics::Record(
            L"RUN FAILED: final import binding left " +
            std::to_wstring(m_bindings.unresolved) + L" unresolved imports.");
        return false;
    }
    m_bindings = std::move(finalBindings);
    if (!m_runtime.CopyFrom(m_mapped, &bindingError) ||
        !m_runtime.FinalizeProtections(m_mapped, &bindingError))
    {
        if (m_storage) m_storage->CloseAll();
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, bindingError);
        RuntimeDiagnostics::Record(
            L"RUN FAILED: final image copy or page protections: " + bindingError);
        return false;
    }
    RuntimeDiagnostics::Record(
        L"RUN: final import binding completed; " +
        std::to_wstring(m_bindings.bound) + L" regular/delay imports bound.");

    std::wstring tlsError;
    if (!m_runtime.NotifyTls(DLL_PROCESS_ATTACH, &tlsError))
    {
        if (m_storage) m_storage->CloseAll();
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, L"The main image TLS process-attach notification failed: " + tlsError);
        RuntimeDiagnostics::Record(L"RUN FAILED: main-image TLS process attach: " + tlsError);
        return false;
    }
    bool mainTlsAttached = true;
    const auto detachMainTls = [&]()
    {
        if (!mainTlsAttached) return;
        std::wstring ignored;
        if (!m_runtime.NotifyTls(DLL_PROCESS_DETACH, &ignored) && !ignored.empty())
            RuntimeDiagnostics::Record(L"TLS: main-image process detach failed: " + ignored);
        mainTlsAttached = false;
    };

    using GuestEntryPoint = int(WINAPI*)();
    const auto entryPoint = reinterpret_cast<GuestEntryPoint>(m_runtime.Base() + m_mapped.entryPointRva);
    try
    {
        RuntimeDiagnostics::Record(L"RUN: entering guest entry point.");
        GuestExceptionDetails guestException;
        if (!InvokeGuestEntryPoint(entryPoint, exitCode, &guestException))
        {
            if (guestException.requestedExit)
            {
                *exitCode = static_cast<int>(guestException.requestedExitCode);
            }
            else
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
                detachMainTls();
                SetError(error, L"The guest raised a structured exception: " +
                    exceptionDescription + L".");
                RuntimeDiagnostics::Record(L"RUN FAILED: guest structured exception " +
                    exceptionDescription + L".");
                return false;
            }
        }
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        detachMainTls();
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
        detachMainTls();
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
        detachMainTls();
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
    child->SetRegistryContext(m_registry);

    const std::shared_ptr<GuestKernelContext> completionKernel = m_kernel;
    const DWORD processId = g_nextGuestProcessId.fetch_add(1);
    const DWORD threadId = g_nextGuestProcessId.fetch_add(1);
    HANDLE processHandle = completionKernel->CreateCompletionObject(processId, true, &errorCode);
    if (!processHandle)
    {
        if (win32Error) *win32Error = errorCode;
        return false;
    }
    HANDLE threadHandle = completionKernel->CreateCompletionObject(threadId, false, &errorCode);
    if (!threadHandle)
    {
        completionKernel->CloseHandle(processHandle, nullptr);
        if (win32Error) *win32Error = errorCode;
        return false;
    }

    child->SetProcessId(processId);
    child->SetMainThreadId(threadId);
    processInformation->hProcess = processHandle;
    processInformation->hThread = threadHandle;
    processInformation->dwProcessId = processId;
    processInformation->dwThreadId = threadId;

    RuntimeDiagnostics::Record(
        L"PROCESS: created asynchronous cooperative child guest for " + logicalPath + L".");
    const std::shared_ptr<GuestWindowManager> childWindows = m_windows;
    const HWND previousForeground = childWindows->GetGuestForegroundWindow();
    DWORD activationError = ERROR_SUCCESS;
    const HWND previousFocus = childWindows->GetGuestFocus(&activationError);
    try
    {
        std::thread([child, completionKernel, childWindows, processHandle, threadHandle,
            processId, logicalPath, previousForeground, previousFocus]()
        {
            int exitCode = static_cast<int>(ERROR_PROCESS_ABORTED);
            std::wstring childError;
            const bool completed = child->Run(&exitCode, &childError);
            childWindows->DestroyGuestWindowsForProcess(
                processId, previousForeground, previousFocus);
            const DWORD reportedExitCode = completed
                ? static_cast<DWORD>(exitCode) : ERROR_PROCESS_ABORTED;
            DWORD ignored = ERROR_SUCCESS;
            completionKernel->CompleteObject(threadHandle, reportedExitCode, &ignored);
            completionKernel->CompleteObject(processHandle, reportedExitCode, &ignored);
            RuntimeDiagnostics::Record(
                completed
                    ? L"PROCESS: child returned from " + logicalPath +
                        L" with exit code " + std::to_wstring(exitCode) + L"."
                    : L"PROCESS FAILED: child stopped in " + logicalPath + L": " + childError);
        }).detach();
    }
    catch (const std::system_error&)
    {
        completionKernel->CloseHandle(threadHandle, nullptr);
        completionKernel->CloseHandle(processHandle, nullptr);
        ZeroMemory(processInformation, sizeof(*processInformation));
        if (win32Error) *win32Error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
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

    const DWORD assignedThreadId = g_nextGuestProcessId.fetch_add(1);
    DWORD errorCode = ERROR_SUCCESS;
    HANDLE completion = m_kernel->CreateCompletionObject(
        assignedThreadId, false, &errorCode);
    if (!completion)
    {
        if (win32Error) *win32Error = errorCode;
        return nullptr;
    }

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
            std::wstring tlsError;
            bool modulesThreadAttached =
                modules->NotifyThread(DLL_THREAD_ATTACH, &tlsError);
            bool mainThreadAttached = modulesThreadAttached &&
                runtime->m_runtime.NotifyTls(DLL_THREAD_ATTACH, &tlsError);
            if (!mainThreadAttached)
            {
                exitCode = ERROR_DLL_INIT_FAILED;
                RuntimeDiagnostics::Record(
                    L"THREAD FAILED: TLS/DLL thread attach notification: " + tlsError);
            }
            else if (!InvokeGuestThreadEntry(
                startAddress, parameter, &exitCode, &exception))
            {
                if (exception.requestedExit)
                {
                    exitCode = exception.requestedExitCode;
                }
                else
                {
                    exitCode = ERROR_PROCESS_ABORTED;
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
            }
            if (modulesThreadAttached)
            {
                std::wstring detachError;
                if (!modules->NotifyThread(DLL_THREAD_DETACH, &detachError) && !detachError.empty())
                    RuntimeDiagnostics::Record(L"THREAD: DLL thread detach failed: " + detachError);
            }
            if (mainThreadAttached)
            {
                std::wstring detachError;
                if (!runtime->m_runtime.NotifyTls(DLL_THREAD_DETACH, &detachError) && !detachError.empty())
                    RuntimeDiagnostics::Record(L"THREAD: main-image TLS thread detach failed: " + detachError);
            }
            DWORD ignored = ERROR_SUCCESS;
            kernel->CompleteObject(completion, exitCode, &ignored);
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
