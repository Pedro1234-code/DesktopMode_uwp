#pragma once

#include "Bridge\\CompatibilityCatalog.h"

#include <windows.h>
#include <string>

namespace Win32Bridge
{
namespace Bridge
{
    class GuestCommandLineScope final
    {
    public:
        explicit GuestCommandLineScope(
            const std::wstring& commandLine,
            DWORD processId = 1,
            DWORD threadId = 0);
        ~GuestCommandLineScope();

        GuestCommandLineScope(const GuestCommandLineScope&) = delete;
        GuestCommandLineScope& operator=(const GuestCommandLineScope&) = delete;

    private:
        std::wstring m_previous;
        DWORD m_previousProcessId;
        DWORD m_previousThreadId;
    };

    void ResetGuestResourceHandles();
    HANDLE WINAPI BridgeCreateFileW(
        LPCWSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile);
    BOOL WINAPI BridgeReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead, LPDWORD bytesRead, LPOVERLAPPED overlapped);
    BOOL WINAPI BridgeWriteFile(HANDLE file, LPCVOID buffer, DWORD bytesToWrite, LPDWORD bytesWritten, LPOVERLAPPED overlapped);
    BOOL WINAPI BridgeCloseHandle(HANDLE object);
    BOOL WINAPI BridgeGetFileSizeEx(HANDLE file, PLARGE_INTEGER fileSize);
    DWORD WINAPI BridgeGetFileSize(HANDLE file, LPDWORD fileSizeHigh);
    BOOL WINAPI BridgeSetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER newPosition, DWORD moveMethod);
    DWORD WINAPI BridgeSetFilePointer(HANDLE file, LONG distance, PLONG distanceHigh, DWORD moveMethod);
    BOOL WINAPI BridgeFlushFileBuffers(HANDLE file);
    BOOL WINAPI BridgeSetEndOfFile(HANDLE file);
    HANDLE WINAPI BridgeOpenEventW(DWORD desiredAccess, BOOL inheritHandle, LPCWSTR name);
    HANDLE WINAPI BridgeOpenFileMappingW(DWORD desiredAccess, BOOL inheritHandle, LPCWSTR name);
    LPVOID WINAPI BridgeMapViewOfFile(HANDLE mapping, DWORD desiredAccess, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytesToMap);
    BOOL WINAPI BridgeUnmapViewOfFile(LPCVOID baseAddress);
    BOOL WINAPI BridgeGetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information);
    BOOL WINAPI BridgeSetFileTime(HANDLE file, const FILETIME* creation, const FILETIME* access, const FILETIME* write);
    BOOL WINAPI BridgeFileTimeToDosDateTime(const FILETIME* fileTime, LPWORD date, LPWORD time);
    BOOL WINAPI BridgeFileTimeToLocalFileTime(const FILETIME* fileTime, LPFILETIME localFileTime);
    BOOL WINAPI BridgeLocalFileTimeToFileTime(const FILETIME* localFileTime, LPFILETIME fileTime);
    BOOL WINAPI BridgeDosDateTimeToFileTime(WORD date, WORD time, LPFILETIME fileTime);
    UINT WINAPI BridgeGetOEMCP();
    BOOL WINAPI BridgeFileTimeToSystemTime(const FILETIME* fileTime, LPSYSTEMTIME systemTime);
    LONG WINAPI BridgeCompareFileTime(const FILETIME* first, const FILETIME* second);
    BOOL WINAPI BridgeGetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
    BOOL WINAPI BridgeGetProcessAffinityMask(HANDLE process, PDWORD_PTR processMask, PDWORD_PTR systemMask);
    DWORD WINAPI BridgeResumeThread(HANDLE thread);
    BOOL WINAPI BridgeTerminateProcess(HANDLE process, UINT exitCode);
    BOOL WINAPI BridgeDeviceIoControl(HANDLE device, DWORD controlCode, LPVOID inBuffer, DWORD inSize, LPVOID outBuffer, DWORD outSize, LPDWORD bytesReturned, LPOVERLAPPED overlapped);
    HANDLE WINAPI BridgeFindFirstStreamW(LPCWSTR fileName, DWORD infoLevel, LPVOID findData, DWORD flags);
    BOOL WINAPI BridgeFindNextStreamW(HANDLE findHandle, LPVOID findData);
    BOOL WINAPI BridgeCreateHardLinkW(LPCWSTR fileName, LPCWSTR existingFileName, LPSECURITY_ATTRIBUTES attributes);
    BOOL WINAPI BridgeMoveFileWithProgressW(LPCWSTR existingFileName, LPCWSTR newFileName, LPPROGRESS_ROUTINE progress, LPVOID data, DWORD flags);
    DWORD WINAPI BridgeFormatMessageW(DWORD flags, LPCVOID source, DWORD messageId, DWORD languageId, LPWSTR buffer, DWORD size, va_list* arguments);
    HLOCAL WINAPI BridgeLocalFree(HLOCAL memory);
    HLOCAL WINAPI BridgeLocalAlloc(UINT flags, SIZE_T bytes);
    LONG WINAPI BridgeUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo);
    LPTOP_LEVEL_EXCEPTION_FILTER WINAPI BridgeSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
    PRUNTIME_FUNCTION WINAPI BridgeRtlLookupFunctionEntry(DWORD64 controlPc, PDWORD64 imageBase, PUNWIND_HISTORY_TABLE historyTable);
    void WINAPI BridgeRtlVirtualUnwind(DWORD handlerType, DWORD64 imageBase, DWORD64 controlPc, PRUNTIME_FUNCTION functionEntry, PCONTEXT contextRecord, PVOID* handlerData, PDWORD64 establisherFrame, PKNONVOLATILE_CONTEXT_POINTERS contextPointers);
    void WINAPI BridgeRtlCaptureContext(PCONTEXT contextRecord);
    HANDLE WINAPI BridgeOpenProcess(DWORD desiredAccess, BOOL inheritHandle, DWORD processId);
    HANDLE WINAPI BridgeCreateToolhelp32Snapshot(DWORD flags, DWORD processId);
    BOOL WINAPI BridgeProcess32FirstW(HANDLE snapshot, LPVOID entry);
    BOOL WINAPI BridgeProcess32NextW(HANDLE snapshot, LPVOID entry);
    BOOL WINAPI BridgeCopyFileExW(LPCWSTR existingFileName, LPCWSTR newFileName, LPPROGRESS_ROUTINE progress, LPVOID data, LPBOOL cancel, DWORD flags);
    DWORD WINAPI BridgeGetCompressedFileSizeW(LPCWSTR fileName, LPDWORD fileSizeHigh);
    HANDLE WINAPI BridgeFindFirstChangeNotificationW(LPCWSTR path, BOOL watchSubtree, DWORD filter);
    BOOL WINAPI BridgeFindNextChangeNotification(HANDLE changeHandle);
    BOOL WINAPI BridgeFindCloseChangeNotification(HANDLE changeHandle);
    HANDLE WINAPI BridgeCreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protection, DWORD maximumSizeHigh, DWORD maximumSizeLow, LPCWSTR name);
    BOOL WINAPI BridgeCreateProcessW(LPCWSTR applicationName, LPWSTR commandLine, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environment, LPCWSTR currentDirectory, LPSTARTUPINFOW startupInfo, LPPROCESS_INFORMATION processInformation);
    HANDLE WINAPI BridgeCreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE startAddress, LPVOID parameter, DWORD creationFlags, LPDWORD threadId);
    void WINAPI BridgeRaiseException(DWORD exceptionCode, DWORD exceptionFlags, DWORD argumentCount, const ULONG_PTR* arguments);
    BOOL WINAPI BridgeCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES securityAttributes);
    BOOL WINAPI BridgeDeleteFileW(LPCWSTR path);
    BOOL WINAPI BridgeMoveFileW(LPCWSTR existingFileName, LPCWSTR newFileName);
    BOOL WINAPI BridgeMoveFileExW(LPCWSTR existingFileName, LPCWSTR newFileName, DWORD flags);
    BOOL WINAPI BridgeRemoveDirectoryW(LPCWSTR path);
    DWORD WINAPI BridgeGetFileAttributesW(LPCWSTR path);
    BOOL WINAPI BridgeSetFileAttributesW(LPCWSTR path, DWORD attributes);
    HANDLE WINAPI BridgeFindFirstFileW(LPCWSTR searchPattern, LPWIN32_FIND_DATAW findData);
    BOOL WINAPI BridgeFindNextFileW(HANDLE findHandle, LPWIN32_FIND_DATAW findData);
    BOOL WINAPI BridgeFindClose(HANDLE findHandle);
    BOOL WINAPI BridgeSetCurrentDirectoryW(LPCWSTR path);
    DWORD WINAPI BridgeGetCurrentDirectoryW(DWORD bufferLength, LPWSTR buffer);
    DWORD WINAPI BridgeGetTempPathW(DWORD bufferLength, LPWSTR buffer);
    DWORD WINAPI BridgeGetLogicalDrives();
    DWORD WINAPI BridgeGetLogicalDriveStringsW(DWORD bufferLength, LPWSTR buffer);
    UINT WINAPI BridgeGetDriveTypeW(LPCWSTR rootPath);
    BOOL WINAPI BridgeGetDiskFreeSpaceExW(LPCWSTR rootPath, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER freeBytes);
    BOOL WINAPI BridgeGetDiskFreeSpaceW(LPCWSTR rootPath, LPDWORD sectorsPerCluster, LPDWORD bytesPerSector, LPDWORD freeClusters, LPDWORD totalClusters);
    BOOL WINAPI BridgeGetVolumeInformationW(LPCWSTR rootPath, LPWSTR volumeName, DWORD volumeNameSize, LPDWORD serial, LPDWORD maximumComponentLength, LPDWORD fileSystemFlags, LPWSTR fileSystemName, DWORD fileSystemNameSize);
    UINT WINAPI BridgeGetWindowsDirectoryW(LPWSTR buffer, UINT bufferLength);
    DWORD WINAPI BridgeExpandEnvironmentStringsW(LPCWSTR source, LPWSTR destination, DWORD destinationLength);
    DWORD WINAPI BridgeGetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD bufferLength);
    HMODULE WINAPI BridgeGetModuleHandleW(LPCWSTR moduleName);
    HMODULE WINAPI BridgeGetModuleHandleA(LPCSTR moduleName);
    HRSRC WINAPI BridgeFindResourceA(HMODULE module, LPCSTR name, LPCSTR type);
    HRSRC WINAPI BridgeFindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type);
    HRSRC WINAPI BridgeFindResourceExA(HMODULE module, LPCSTR type, LPCSTR name, WORD language);
    HRSRC WINAPI BridgeFindResourceExW(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language);
    HGLOBAL WINAPI BridgeLoadResource(HMODULE module, HRSRC resource);
    LPVOID WINAPI BridgeLockResource(HGLOBAL resource);
    DWORD WINAPI BridgeSizeofResource(HMODULE module, HRSRC resource);
    BOOL WINAPI BridgeFreeResource(HGLOBAL resource);
    BOOL WINAPI BridgeEnumResourceTypesA(HMODULE module, ENUMRESTYPEPROCA callback, LONG_PTR parameter);
    BOOL WINAPI BridgeEnumResourceTypesW(HMODULE module, ENUMRESTYPEPROCW callback, LONG_PTR parameter);
    BOOL WINAPI BridgeEnumResourceTypesExA(HMODULE module, ENUMRESTYPEPROCA callback, LONG_PTR parameter, DWORD flags, LANGID language);
    BOOL WINAPI BridgeEnumResourceTypesExW(HMODULE module, ENUMRESTYPEPROCW callback, LONG_PTR parameter, DWORD flags, LANGID language);
    BOOL WINAPI BridgeEnumResourceNamesA(HMODULE module, LPCSTR type, ENUMRESNAMEPROCA callback, LONG_PTR parameter);
    BOOL WINAPI BridgeEnumResourceNamesW(HMODULE module, LPCWSTR type, ENUMRESNAMEPROCW callback, LONG_PTR parameter);
    BOOL WINAPI BridgeEnumResourceNamesExA(HMODULE module, LPCSTR type, ENUMRESNAMEPROCA callback, LONG_PTR parameter, DWORD flags, LANGID language);
    BOOL WINAPI BridgeEnumResourceNamesExW(HMODULE module, LPCWSTR type, ENUMRESNAMEPROCW callback, LONG_PTR parameter, DWORD flags, LANGID language);
    BOOL WINAPI BridgeEnumResourceLanguagesA(HMODULE module, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA callback, LONG_PTR parameter);
    BOOL WINAPI BridgeEnumResourceLanguagesW(HMODULE module, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW callback, LONG_PTR parameter);
    BOOL WINAPI BridgeEnumResourceLanguagesExA(HMODULE module, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA callback, LONG_PTR parameter, DWORD flags, LANGID language);
    BOOL WINAPI BridgeEnumResourceLanguagesExW(HMODULE module, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW callback, LONG_PTR parameter, DWORD flags, LANGID language);
    DWORD WINAPI BridgeGetTickCount();
    ULONGLONG WINAPI BridgeGetTickCount64();
    BOOL WINAPI BridgeQueryPerformanceCounter(PLARGE_INTEGER counter);
    BOOL WINAPI BridgeQueryPerformanceFrequency(PLARGE_INTEGER frequency);
    DWORD WINAPI BridgeGetCurrentThreadId();
    DWORD WINAPI BridgeGetCurrentProcessId();
    HANDLE WINAPI BridgeGetCurrentProcess();
    void WINAPI BridgeGetSystemTimeAsFileTime(LPFILETIME systemTime);
    LPVOID WINAPI BridgeVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protection);
    BOOL WINAPI BridgeVirtualFree(LPVOID address, SIZE_T size, DWORD freeType);
    BOOL WINAPI BridgeVirtualProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD oldProtection);
    SIZE_T WINAPI BridgeGetLargePageMinimum();
    HGLOBAL WINAPI BridgeGlobalAlloc(UINT flags, SIZE_T bytes);
    HGLOBAL WINAPI BridgeGlobalFree(HGLOBAL memory);
    LPVOID WINAPI BridgeGlobalLock(HGLOBAL memory);
    BOOL WINAPI BridgeGlobalUnlock(HGLOBAL memory);
    SIZE_T WINAPI BridgeGlobalSize(HGLOBAL memory);
    HANDLE WINAPI BridgeGetProcessHeap();
    LPVOID WINAPI BridgeHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
    BOOL WINAPI BridgeHeapFree(HANDLE heap, DWORD flags, LPVOID memory);
    void WINAPI BridgeInitializeCriticalSection(LPCRITICAL_SECTION criticalSection);
    void WINAPI BridgeEnterCriticalSection(LPCRITICAL_SECTION criticalSection);
    void WINAPI BridgeLeaveCriticalSection(LPCRITICAL_SECTION criticalSection);
    void WINAPI BridgeDeleteCriticalSection(LPCRITICAL_SECTION criticalSection);
    void WINAPI BridgeGetSystemInfo(LPSYSTEM_INFO systemInfo);
    BOOL WINAPI BridgeGlobalMemoryStatusEx(LPMEMORYSTATUSEX memoryStatus);
    BOOL WINAPI BridgeIsProcessorFeaturePresent(DWORD feature);
    int WINAPI BridgeLstrlenW(LPCWSTR text);
    LPWSTR WINAPI BridgeLstrcatW(LPWSTR destination, LPCWSTR source);
    int WINAPI BridgeWideCharToMultiByte(
        UINT codePage, DWORD flags, LPCWCH wideCharacters, int wideCharacterCount,
        LPSTR multiByte, int multiByteCount, LPCCH defaultCharacter, LPBOOL usedDefaultCharacter);
    int WINAPI BridgeMultiByteToWideChar(
        UINT codePage, DWORD flags, LPCCH multiByte, int multiByteCount,
        LPWSTR wideCharacters, int wideCharacterCount);
    LANGID WINAPI BridgeGetSystemDefaultLangID();
    LANGID WINAPI BridgeGetUserDefaultLangID();
    DWORD WINAPI BridgeGetVersion();
    void WINAPI BridgeGetStartupInfoA(LPSTARTUPINFOA startupInfo);
    HANDLE WINAPI BridgeGetStdHandle(DWORD standardHandle);
    BOOL WINAPI BridgeGetConsoleMode(HANDLE console, LPDWORD mode);
    BOOL WINAPI BridgeSetPriorityClass(HANDLE process, DWORD priorityClass);
    BOOL WINAPI BridgeSetProcessAffinityMask(HANDLE process, DWORD_PTR processMask);
    DWORD_PTR WINAPI BridgeSetThreadAffinityMask(HANDLE thread, DWORD_PTR threadMask);
    BOOL WINAPI BridgeSetDefaultDllDirectories(DWORD directoryFlags);
    HMODULE WINAPI BridgeLoadLibraryW(LPCWSTR fileName);
    HMODULE WINAPI BridgeLoadLibraryA(LPCSTR fileName);
    HMODULE WINAPI BridgeLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags);
    FARPROC WINAPI BridgeGetProcAddress(HMODULE module, LPCSTR nameOrOrdinal);
    BOOL WINAPI BridgeFreeLibrary(HMODULE module);
    LPWSTR WINAPI BridgeGetCommandLineW();
    void WINAPI BridgeOutputDebugStringW(LPCWSTR message);
    BOOL WINAPI BridgeIsDebuggerPresent();
    DWORD WINAPI BridgeGetLastError();
    void WINAPI BridgeSetLastError(DWORD error);
    HANDLE WINAPI BridgeCreateEventW(
        LPSECURITY_ATTRIBUTES eventAttributes,
        BOOL manualReset,
        BOOL initialState,
        LPCWSTR name);
    BOOL WINAPI BridgeSetEvent(HANDLE eventHandle);
    BOOL WINAPI BridgeResetEvent(HANDLE eventHandle);
    HANDLE WINAPI BridgeCreateMutexW(
        LPSECURITY_ATTRIBUTES mutexAttributes,
        BOOL initialOwner,
        LPCWSTR name);
    BOOL WINAPI BridgeReleaseMutex(HANDLE mutexHandle);
    HANDLE WINAPI BridgeCreateSemaphoreW(
        LPSECURITY_ATTRIBUTES semaphoreAttributes,
        LONG initialCount,
        LONG maximumCount,
        LPCWSTR name);
    BOOL WINAPI BridgeReleaseSemaphore(HANDLE semaphoreHandle, LONG releaseCount, LPLONG previousCount);
    DWORD WINAPI BridgeWaitForSingleObject(HANDLE object, DWORD milliseconds);
    DWORD WINAPI BridgeWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds);
    void WINAPI BridgeSleep(DWORD milliseconds);

    ImportResolution ResolveKernel32Import(const ImportedSymbol& symbol);
}
}
