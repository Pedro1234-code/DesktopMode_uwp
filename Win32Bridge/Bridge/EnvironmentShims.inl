// Kernel32 environment adapters. All data comes from the active guest loader.
namespace
{
    Win32Bridge::Bridge::ProcessEnvironment* CurrentProcessEnvironment()
    {
        auto loader = CurrentGuestModuleLoader();
        if (!loader) SetGuestLastError(ERROR_INVALID_FUNCTION);
        return loader ? &loader->Environment() : nullptr;
    }
    bool EnvironmentWide(const char* source, std::wstring* result)
    {
        if (!source) return false;
        const int required = ::MultiByteToWideChar(CP_ACP, 0, source, -1, nullptr, 0);
        if (!required) return false;
        result->resize(static_cast<size_t>(required));
        if (!::MultiByteToWideChar(CP_ACP, 0, source, -1, &(*result)[0], required)) return false;
        result->resize(static_cast<size_t>(required - 1)); return true;
    }
    template<class Character> DWORD CopyEnvironmentValue(const std::basic_string<Character>& value, Character* buffer, DWORD length, bool expansion)
    {
        if (value.size() >= MAXDWORD) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        const DWORD required = static_cast<DWORD>(value.size()) + 1;
        if (length < required) return required;
        if (!buffer) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
        memcpy(buffer, value.c_str(), required * sizeof(Character));
        SetGuestLastError(ERROR_SUCCESS);
        return expansion ? required : required - 1;
    }
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD length)
{
    if (!name) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    auto environment = CurrentProcessEnvironment(); if (!environment) return 0;
    try
    {
        std::wstring value;
        if (!environment->Get(name, &value)) { SetGuestLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
        return CopyEnvironmentValue(value, buffer, length, false);
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
}
DWORD WINAPI Win32Bridge::Bridge::BridgeGetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD length)
{
    auto environment = CurrentProcessEnvironment(); if (!environment) return 0;
    try
    {
        std::wstring wideName, value;
        if (!EnvironmentWide(name, &wideName)) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
        if (!environment->Get(wideName, &value)) { SetGuestLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
        return CopyEnvironmentValue(CrtNarrowString(value), buffer, length, false);
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
}
BOOL WINAPI Win32Bridge::Bridge::BridgeSetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
    auto environment = CurrentProcessEnvironment(); if (!environment) return FALSE;
    try
    {
        const DWORD error = environment->Set(name, value);
        SetGuestLastError(error); return error == ERROR_SUCCESS;
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
}
BOOL WINAPI Win32Bridge::Bridge::BridgeSetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    try
    {
        std::wstring wideName, wideValue;
        if (!EnvironmentWide(name, &wideName) || (value && !EnvironmentWide(value, &wideValue)))
        { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        return BridgeSetEnvironmentVariableW(wideName.c_str(), value ? wideValue.c_str() : nullptr);
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
}
DWORD WINAPI Win32Bridge::Bridge::BridgeExpandEnvironmentStringsW(LPCWSTR source, LPWSTR buffer, DWORD length)
{
    if (!source) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    auto environment = CurrentProcessEnvironment(); if (!environment) return 0;
    try { return CopyEnvironmentValue(environment->Expand(source), buffer, length, true); }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
}
DWORD WINAPI Win32Bridge::Bridge::BridgeExpandEnvironmentStringsA(LPCSTR source, LPSTR buffer, DWORD length)
{
    auto environment = CurrentProcessEnvironment(); if (!environment) return 0;
    try
    {
        std::wstring wide;
        if (!EnvironmentWide(source, &wide)) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
        return CopyEnvironmentValue(CrtNarrowString(environment->Expand(wide)), buffer, length, true);
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
}
LPWCH WINAPI Win32Bridge::Bridge::BridgeGetEnvironmentStringsW()
{
    auto environment = CurrentProcessEnvironment(); if (!environment) return nullptr;
    try
    {
        const auto block = environment->Block();
        auto memory = static_cast<LPWCH>(::HeapAlloc(::GetProcessHeap(), 0, block.size() * sizeof(wchar_t)));
        if (!memory) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        memcpy(memory, block.data(), block.size() * sizeof(wchar_t)); return memory;
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
}
LPCH WINAPI Win32Bridge::Bridge::BridgeGetEnvironmentStringsA()
{
    auto environment = CurrentProcessEnvironment(); if (!environment) return nullptr;
    try
    {
        std::vector<char> block;
        for (const auto& entry : environment->Snapshot())
        {
            const auto text = CrtNarrowString(entry);
            block.insert(block.end(), text.begin(), text.end()); block.push_back(0);
        }
        if (block.empty()) block.push_back(0);
        block.push_back(0);
        auto memory = static_cast<LPCH>(::HeapAlloc(::GetProcessHeap(), 0, block.size()));
        if (!memory) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        memcpy(memory, block.data(), block.size()); return memory;
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
}
BOOL WINAPI Win32Bridge::Bridge::BridgeFreeEnvironmentStringsW(LPWCH environment)
{
    if (!environment) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const BOOL result = ::HeapFree(::GetProcessHeap(), 0, environment);
    if (!result) SetGuestLastError(ERROR_INVALID_PARAMETER);
    return result;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeFreeEnvironmentStringsA(LPCH environment)
{
    return BridgeFreeEnvironmentStringsW(reinterpret_cast<LPWCH>(environment));
}