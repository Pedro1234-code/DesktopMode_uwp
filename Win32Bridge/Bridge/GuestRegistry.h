#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Win32Bridge { namespace Bridge {
    class GuestStorageContext;

    // Logical, guest-only registry. Its API is independent of the physical
    // hive format so the current compact store can later be replaced by REGF.
    class GuestRegistryContext final {
    public:
        struct Hive { const wchar_t* root; const wchar_t* file; };

        explicit GuestRegistryContext(const std::shared_ptr<GuestStorageContext>& storage);
        ~GuestRegistryContext();
        bool Initialize(DWORD* error);
        bool OpenKey(HKEY parent, LPCWSTR subKey, REGSAM access, HKEY* result, DWORD* error);
        bool CreateKey(HKEY parent, LPCWSTR subKey, REGSAM access, HKEY* result, DWORD* disposition, DWORD* error);
        bool QueryValue(HKEY key, LPCWSTR name, LPDWORD type, LPBYTE data, LPDWORD bytes, DWORD* error);
        bool SetValue(HKEY key, LPCWSTR name, DWORD type, const BYTE* data, DWORD bytes, DWORD* error);
        bool DeleteValue(HKEY key, LPCWSTR name, DWORD* error);
        bool DeleteKey(HKEY parent, LPCWSTR subKey, DWORD* error);
        bool EnumKey(HKEY key, DWORD index, LPWSTR name, LPDWORD characters, DWORD* error);
        bool EnumValue(HKEY key, DWORD index, LPWSTR name, LPDWORD characters,
            LPDWORD type, LPBYTE data, LPDWORD bytes, DWORD* error);
        bool QueryInfoKey(HKEY key, LPDWORD subKeyCount, LPDWORD maximumSubKeyLength,
            LPDWORD valueCount, LPDWORD maximumValueNameLength, LPDWORD maximumValueDataLength,
            PFILETIME lastWriteTime, DWORD* error);
        bool Flush(DWORD* error);
        bool CloseKey(HKEY key, DWORD* error);

    private:
        struct Value { std::wstring name; DWORD type = REG_NONE; std::vector<BYTE> data; };
        struct Key { std::wstring path; std::wstring displayPath; std::unordered_map<std::wstring, Value> values; };
        struct Handle { std::wstring path; REGSAM access = 0; };

        bool EnsureInitializedLocked(DWORD* error);
        bool ResolveLocked(HKEY key, std::wstring* path, REGSAM* access, DWORD* error) const;
        bool ComposePathLocked(HKEY parent, LPCWSTR subKey, std::wstring* path,
            std::wstring* displayPath, REGSAM* parentAccess, DWORD* error) const;
        bool LoadHiveLocked(const Hive& hive, bool* existed, DWORD* error);
        bool SaveHiveLocked(const Hive& hive, DWORD* error) const;
        bool SavePathLocked(const std::wstring& path, DWORD* error) const;
        bool SaveAllLocked(DWORD* error) const;
        void SeedDefaultsLocked();
        void EnsureKeyHierarchyLocked(const std::wstring& path, const std::wstring& displayPath);
        const Hive* HiveForPath(const std::wstring& path) const;
        static std::wstring Normalize(LPCWSTR value);
        static std::wstring NormalizePath(const std::wstring& value);
        static std::wstring CanonicalizeAlias(const std::wstring& value);
        static bool IsPathOrChild(const std::wstring& path, const std::wstring& root);

        std::shared_ptr<GuestStorageContext> m_storage;
        mutable std::mutex m_lock;
        std::unordered_map<std::wstring, Key> m_keys;
        std::unordered_map<ULONG_PTR, Handle> m_handles;
        ULONG_PTR m_nextHandle = 0x50000000;
        bool m_initialized = false;
    };

    GuestRegistryContext* CurrentGuestRegistryContext();
    class GuestRegistryScope final { public: explicit GuestRegistryScope(GuestRegistryContext*); ~GuestRegistryScope(); private: GuestRegistryContext* m_previous; };
} }
