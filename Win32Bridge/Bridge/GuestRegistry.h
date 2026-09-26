#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Win32Bridge { namespace Bridge {
    // Guest-only registry namespace. Predefined hives are virtual and no host
    // registry handle is ever exposed to PE code.
    class GuestRegistryContext final {
    public:
        GuestRegistryContext() = default;
        bool OpenKey(HKEY parent, LPCWSTR subKey, REGSAM access, HKEY* result, DWORD* error);
        bool CreateKey(HKEY parent, LPCWSTR subKey, REGSAM access, HKEY* result, DWORD* disposition, DWORD* error);
        bool QueryValue(HKEY key, LPCWSTR name, LPDWORD type, LPBYTE data, LPDWORD bytes, DWORD* error) const;
        bool SetValue(HKEY key, LPCWSTR name, DWORD type, const BYTE* data, DWORD bytes, DWORD* error);
        bool DeleteValue(HKEY key, LPCWSTR name, DWORD* error);
        bool DeleteKey(HKEY parent, LPCWSTR subKey, DWORD* error);
        bool EnumKey(HKEY key, DWORD index, LPWSTR name, LPDWORD characters, DWORD* error) const;
        bool CloseKey(HKEY key, DWORD* error);
    private:
        struct Value { DWORD type = REG_NONE; std::vector<BYTE> data; };
        struct Key { std::wstring path; std::unordered_map<std::wstring, Value> values; };
        bool Resolve(HKEY key, std::wstring* path, DWORD* error) const;
        static std::wstring Normalize(LPCWSTR value);
        mutable std::mutex m_lock;
        std::unordered_map<std::wstring, Key> m_keys;
        std::unordered_map<ULONG_PTR, std::wstring> m_handles;
        ULONG_PTR m_nextHandle = 0x50000000;
    };

    GuestRegistryContext* CurrentGuestRegistryContext();
    class GuestRegistryScope final { public: explicit GuestRegistryScope(GuestRegistryContext*); ~GuestRegistryScope(); private: GuestRegistryContext* m_previous; };
} }
