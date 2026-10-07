#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    struct GuestAssemblyIdentity final
    {
        std::wstring name;
        std::wstring version;
        std::wstring processorArchitecture;
        std::wstring publicKeyToken;
        std::wstring language;
        std::wstring type;
    };

    struct GuestAssemblyFile final
    {
        std::wstring name;
        std::wstring sourcePath;
    };

    struct GuestAssemblyInfo final
    {
        GuestAssemblyIdentity identity;
        std::wstring manifestPath;
        std::wstring directory;
        std::vector<GuestAssemblyFile> files;
        bool virtualAssembly = false;
    };

    // The Windows SDK hides ACTCTXA/W from AppContainer builds even though
    // guest desktop binaries still pass this ABI to our emulated kernel32.
    // Keep a bridge-owned copy instead of exposing desktop-only host APIs.
    struct GuestActCtxA final
    {
        ULONG cbSize;
        DWORD dwFlags;
        LPCSTR lpSource;
        USHORT wProcessorArchitecture;
        LANGID wLangId;
        LPCSTR lpAssemblyDirectory;
        LPCSTR lpResourceName;
        LPCSTR lpApplicationName;
        HMODULE hModule;
    };

    struct GuestActCtxW final
    {
        ULONG cbSize;
        DWORD dwFlags;
        LPCWSTR lpSource;
        USHORT wProcessorArchitecture;
        LANGID wLangId;
        LPCWSTR lpAssemblyDirectory;
        LPCWSTR lpResourceName;
        LPCWSTR lpApplicationName;
        HMODULE hModule;
    };

    struct GuestManifestInfo final
    {
        std::wstring xml;
        std::wstring source;
        std::wstring assemblyDirectory;
        std::wstring assemblyIdentity;
        GuestAssemblyIdentity identity;
        std::vector<GuestAssemblyIdentity> dependencies;
        std::vector<GuestAssemblyInfo> assemblies;
        std::wstring dpiAwareness;
        std::vector<GUID> supportedOperatingSystems;
        ACTCTX_REQUESTED_RUN_LEVEL runLevel = ACTCTX_RUN_LEVEL_UNSPECIFIED;
        bool uiAccess = false;
        bool commonControlsV6 = false;
        bool longPathAware = false;
    };

    const GuestManifestInfo* CurrentGuestManifest();
    bool CurrentGuestUsesVisualStyles();
    bool ResolveGuestActivationContextModule(LPCWSTR moduleName, std::wstring* path);

    class GuestActivationContextScope final
    {
    public:
        GuestActivationContextScope();
        ~GuestActivationContextScope();
        GuestActivationContextScope(const GuestActivationContextScope&) = delete;
        GuestActivationContextScope& operator=(const GuestActivationContextScope&) = delete;
    private:
        HANDLE m_previous = nullptr;
        HANDLE m_context = nullptr;
    };

    HANDLE WINAPI BridgeCreateActCtxW(const GuestActCtxW* context);
    HANDLE WINAPI BridgeCreateActCtxA(const GuestActCtxA* context);
    void WINAPI BridgeAddRefActCtx(HANDLE context);
    void WINAPI BridgeReleaseActCtx(HANDLE context);
    BOOL WINAPI BridgeActivateActCtx(HANDLE context, ULONG_PTR* cookie);
    BOOL WINAPI BridgeDeactivateActCtx(DWORD flags, ULONG_PTR cookie);
    BOOL WINAPI BridgeGetCurrentActCtx(HANDLE* context);
    BOOL WINAPI BridgeQueryActCtxW(
        DWORD flags,
        HANDLE context,
        PVOID subInstance,
        ULONG informationClass,
        PVOID buffer,
        SIZE_T bufferSize,
        SIZE_T* requiredSize);
    BOOL WINAPI BridgeQueryActCtxSettingsW(
        DWORD flags,
        HANDLE context,
        LPCWSTR nameSpace,
        LPCWSTR setting,
        LPWSTR buffer,
        SIZE_T bufferCount,
        SIZE_T* written);
}
}
