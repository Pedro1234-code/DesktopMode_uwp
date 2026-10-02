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
#include <roapi.h>
#include <system_error>
#include <thread>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local GuestRuntime* g_currentGuestRuntime = nullptr;
    std::atomic<unsigned> g_activeGuestRuntimeScopes{ 0 };
    std::atomic<DWORD> g_nextGuestProcessId{ 1000 };

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

    // This is intentionally a leaf function. A guest PE shares our process,
    // and an SEH failure in its message loop would otherwise terminate the
    // CoreShell host before RuntimeSession can persist diagnostics.
    bool InvokeGuestEntryPoint(int(WINAPI* entryPoint)(), int* exitCode, DWORD* exceptionCode)
    {
        if (exceptionCode)
        {
            *exceptionCode = ERROR_SUCCESS;
        }

        __try
        {
            *exitCode = entryPoint();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode)
            {
                *exceptionCode = GetExceptionCode();
            }
            return false;
        }
    }

    bool InvokeGuestThreadEntry(
        LPTHREAD_START_ROUTINE startAddress,
        LPVOID parameter,
        DWORD* exitCode,
        DWORD* exceptionCode)
    {
        if (exceptionCode) *exceptionCode = ERROR_SUCCESS;
        __try
        {
            *exitCode = startAddress(parameter);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode) *exceptionCode = GetExceptionCode();
            return false;
        }
    }
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
    m_registry = std::make_shared<GuestRegistryContext>();
    GuestKernelScope kernelScope(m_kernel.get());
    GuestModuleScope moduleScope(m_modules.get());
    GuestRegistryScope registryScope(m_registry.get());
    GuestResourceHandleScope resourceHandleScope;
    GuestResourceScope resourceScope(m_runtime.Base(), m_runtime.Size());
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
        DWORD guestException = ERROR_SUCCESS;
        if (!InvokeGuestEntryPoint(entryPoint, exitCode, &guestException))
        {
            if (m_storage)
            {
                m_storage->CloseAll();
            }
            m_kernel->CloseAll();
            m_modules->ReleaseAll();
            SetError(error, L"The guest raised a structured exception (" +
                std::to_wstring(static_cast<unsigned long>(guestException)) + L").");
            RuntimeDiagnostics::Record(L"RUN FAILED: guest structured exception " +
                std::to_wstring(static_cast<unsigned long>(guestException)) + L".");
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
    int exitCode = -1;
    std::wstring childError;
    const bool completed = child->Run(&exitCode, &childError);
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
    const std::wstring commandLine = m_commandLine.empty() && m_storage
        ? L"\"" + m_storage->ModulePath() + L"\""
        : m_commandLine;
    const DWORD processId = m_processId;
    GuestRuntime* const runtime = this;

    try
    {
        std::thread([
            kernel, modules, registry, storage, windows,
            imageBase, imageSize, commandLine, processId, assignedThreadId, runtime,
            completion, startAddress, parameter]()
        {
            const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
            GuestKernelScope kernelScope(kernel.get());
            GuestModuleScope moduleScope(modules.get());
            GuestRegistryScope registryScope(registry.get());
            GuestResourceScope resourceScope(imageBase, imageSize);
            GuestStorageScope storageScope(storage.get());
            GuestActivationContextScope activationContextScope;
            GuestWindowScope windowScope(windows.get(), false);
            CurrentGuestRuntimeScope runtimeScope(runtime);
            GuestCommandLineScope commandLineScope(commandLine, processId, assignedThreadId);

            DWORD exitCode = 0;
            DWORD exceptionCode = ERROR_SUCCESS;
            if (!InvokeGuestThreadEntry(
                startAddress, parameter, &exitCode, &exceptionCode))
            {
                RuntimeDiagnostics::Record(
                    L"THREAD FAILED: guest structured exception " +
                    std::to_wstring(exceptionCode) + L".");
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
