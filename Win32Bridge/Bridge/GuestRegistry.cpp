#include "pch.h"
#include "Bridge/GuestRegistry.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <iterator>
#include <set>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local GuestRegistryContext* g_registry = nullptr;
    constexpr BYTE Magic[8] = { 'W','B','R','H','I','V','E','1' };
    constexpr DWORD FormatVersion = 1;
    constexpr DWORD MaxHiveBytes = 64u * 1024u * 1024u;
    constexpr DWORD MaxKeys = 100000;
    constexpr DWORD MaxNameChars = 32767;
    constexpr DWORD MaxValues = 65536;
    constexpr DWORD MaxValueBytes = 32u * 1024u * 1024u;

    const std::array<GuestRegistryContext::Hive, 6> Hives = {{
        { L"hklm\\system", L"C:\\Windows\\System32\\config\\SYSTEM" },
        { L"hklm\\software", L"C:\\Windows\\System32\\config\\SOFTWARE" },
        { L"hklm\\sam", L"C:\\Windows\\System32\\config\\SAM" },
        { L"hklm\\security", L"C:\\Windows\\System32\\config\\SECURITY" },
        { L"hku\\.default", L"C:\\Windows\\System32\\config\\DEFAULT" },
        { L"hkcu", L"C:\\Users\\Default\\NTUSER.DAT" }
    }};

    void SetError(DWORD* output, DWORD value) { if (output) *output = value; }

    template<typename T> void Append(std::vector<BYTE>* output, T value)
    {
        const BYTE* bytes = reinterpret_cast<const BYTE*>(&value);
        output->insert(output->end(), bytes, bytes + sizeof(value));
    }
    void AppendString(std::vector<BYTE>* output, const std::wstring& value)
    {
        Append<DWORD>(output, static_cast<DWORD>(value.size()));
        const BYTE* bytes = reinterpret_cast<const BYTE*>(value.data());
        output->insert(output->end(), bytes, bytes + value.size() * sizeof(wchar_t));
    }

    class Reader final
    {
    public:
        explicit Reader(const std::vector<BYTE>& bytes) : m_bytes(bytes) {}
        bool Bytes(void* output, size_t count)
        {
            if (m_offset > m_bytes.size() || count > m_bytes.size() - m_offset) return false;
            if (count) memcpy(output, m_bytes.data() + m_offset, count);
            m_offset += count;
            return true;
        }
        template<typename T> bool Scalar(T* output) { return output && Bytes(output, sizeof(T)); }
        bool String(std::wstring* output)
        {
            DWORD count = 0;
            if (!output || !Scalar(&count) || count > MaxNameChars) return false;
            const size_t bytes = static_cast<size_t>(count) * sizeof(wchar_t);
            if (m_offset > m_bytes.size() || bytes > m_bytes.size() - m_offset) return false;
            output->assign(reinterpret_cast<const wchar_t*>(m_bytes.data() + m_offset), count);
            m_offset += bytes;
            return true;
        }
        bool End() const { return m_offset == m_bytes.size(); }
    private:
        const std::vector<BYTE>& m_bytes;
        size_t m_offset = 0;
    };

    bool HasAccess(REGSAM granted, REGSAM required)
    {
        return granted == KEY_ALL_ACCESS || (granted & MAXIMUM_ALLOWED) || (granted & required) == required;
    }
    bool IsPredefined(HKEY key)
    {
        return key == HKEY_CURRENT_USER || key == HKEY_LOCAL_MACHINE || key == HKEY_USERS ||
            key == HKEY_CLASSES_ROOT || key == HKEY_CURRENT_CONFIG;
    }
}

GuestRegistryContext::GuestRegistryContext(const std::shared_ptr<GuestStorageContext>& storage) : m_storage(storage) {}
GuestRegistryContext::~GuestRegistryContext() { DWORD ignored = 0; Flush(&ignored); }

std::wstring GuestRegistryContext::Normalize(LPCWSTR source)
{
    std::wstring value = source ? source : L"";
    for (auto& character : value)
    {
        if (character == L'/') character = L'\\';
        character = static_cast<wchar_t>(towlower(character));
    }
    return value;
}

std::wstring GuestRegistryContext::NormalizePath(const std::wstring& source)
{
    std::wstring result;
    bool separator = true;
    for (wchar_t character : source)
    {
        if (character == L'/' || character == L'\\')
        {
            if (!separator) result.push_back(L'\\');
            separator = true;
        }
        else
        {
            result.push_back(static_cast<wchar_t>(towlower(character)));
            separator = false;
        }
    }
    if (!result.empty() && result.back() == L'\\') result.pop_back();
    return result;
}

std::wstring GuestRegistryContext::CanonicalizeAlias(const std::wstring& source)
{
    const std::wstring path = NormalizePath(source);
    const std::wstring alias = L"hku\\win32bridge";
    if (path == alias) return L"hkcu";
    if (path.size() > alias.size() && path.compare(0, alias.size(), alias) == 0 && path[alias.size()] == L'\\')
        return L"hkcu" + path.substr(alias.size());
    return path;
}

bool GuestRegistryContext::IsPathOrChild(const std::wstring& path, const std::wstring& root)
{
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == L'\\');
}

const GuestRegistryContext::Hive* GuestRegistryContext::HiveForPath(const std::wstring& path) const
{
    for (const auto& hive : Hives) if (IsPathOrChild(path, hive.root)) return &hive;
    return nullptr;
}

bool GuestRegistryContext::ResolveLocked(HKEY key, std::wstring* path, REGSAM* access, DWORD* error) const
{
    if (!path) { SetError(error, ERROR_INVALID_PARAMETER); return false; }
    if (key == HKEY_CURRENT_USER) *path = L"hkcu";
    else if (key == HKEY_LOCAL_MACHINE) *path = L"hklm";
    else if (key == HKEY_USERS) *path = L"hku";
    else if (key == HKEY_CLASSES_ROOT) *path = L"hklm\\software\\classes";
    else if (key == HKEY_CURRENT_CONFIG) *path = L"hklm\\system\\currentcontrolset\\hardware profiles\\current";
    else
    {
        const auto found = m_handles.find(reinterpret_cast<ULONG_PTR>(key));
        if (found == m_handles.end()) { SetError(error, ERROR_INVALID_HANDLE); return false; }
        *path = found->second.path;
        if (access) *access = found->second.access;
        return true;
    }
    if (access) *access = KEY_ALL_ACCESS;
    return true;
}

bool GuestRegistryContext::ComposePathLocked(HKEY parent, LPCWSTR subKey, std::wstring* path,
    std::wstring* displayPath, REGSAM* parentAccess, DWORD* error) const
{
    std::wstring base;
    if (!path || !ResolveLocked(parent, &base, parentAccess, error)) return false;
    std::wstring child = subKey ? subKey : L"";
    while (!child.empty() && (child.front() == L'\\' || child.front() == L'/')) child.erase(child.begin());
    while (!child.empty() && (child.back() == L'\\' || child.back() == L'/')) child.pop_back();
    for (auto& character : child) if (character == L'/') character = L'\\';
    *path = CanonicalizeAlias(child.empty() ? base : base + L"\\" + child);
    if (displayPath) *displayPath = child.empty() ? base : base + L"\\" + child;
    return true;
}

void GuestRegistryContext::EnsureKeyHierarchyLocked(const std::wstring& path, const std::wstring& displayPath)
{
    size_t offset = 0;
    for (;;)
    {
        const size_t separator = path.find(L'\\', offset);
        const size_t length = separator == std::wstring::npos ? path.size() : separator;
        const std::wstring partial = path.substr(0, length);
        Key& key = m_keys[partial];
        key.path = partial;
        if (key.displayPath.empty()) key.displayPath = displayPath.size() >= length ? displayPath.substr(0, length) : partial;
        if (separator == std::wstring::npos) break;
        offset = separator + 1;
    }
}

void GuestRegistryContext::SeedDefaultsLocked()
{
    for (const wchar_t* path : { L"hklm", L"hku", L"hkcu", L"hklm\\system", L"hklm\\software",
        L"hklm\\sam", L"hklm\\security", L"hku\\.default" }) EnsureKeyHierarchyLocked(path, path);
    EnsureKeyHierarchyLocked(L"hkcu\\software\\classes", L"HKCU\\Software\\Classes");
    EnsureKeyHierarchyLocked(L"hklm\\software\\classes", L"HKLM\\Software\\Classes");
    const wchar_t* environment = L"hklm\\system\\currentcontrolset\\control\\session manager\\environment";
    EnsureKeyHierarchyLocked(environment, L"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    EnsureKeyHierarchyLocked(L"hklm\\system\\currentcontrolset\\hardware profiles\\current",
        L"HKLM\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current");
    if (!m_keys[environment].values.count(L"systemroot"))
    {
        Value value; value.name = L"SystemRoot"; value.type = REG_SZ;
        const wchar_t text[] = L"C:\\Windows";
        const BYTE* first = reinterpret_cast<const BYTE*>(text);
        value.data.assign(first, first + sizeof(text));
        m_keys[environment].values.emplace(L"systemroot", std::move(value));
    }
}

bool GuestRegistryContext::LoadHiveLocked(const Hive& hive, bool* existed, DWORD* error)
{
    if (existed) *existed = false;
    std::vector<BYTE> bytes;
    DWORD storageError = 0;
    if (!m_storage || !m_storage->ReadAllBytes(hive.file, &bytes, &storageError))
    {
        if (storageError == ERROR_FILE_NOT_FOUND || storageError == ERROR_PATH_NOT_FOUND) { SetError(error, 0); return true; }
        SetError(error, storageError); return false;
    }
    if (existed) *existed = true;
    if (bytes.size() < sizeof(Magic) + sizeof(DWORD) * 2 || bytes.size() > MaxHiveBytes)
    { SetError(error, ERROR_BAD_FORMAT); return false; }
    Reader reader(bytes); BYTE magic[sizeof(Magic)] = {}; DWORD version = 0, keyCount = 0;
    if (!reader.Bytes(magic, sizeof(magic)) || memcmp(magic, Magic, sizeof(magic)) ||
        !reader.Scalar(&version) || version != FormatVersion || !reader.Scalar(&keyCount) || keyCount > MaxKeys)
    { SetError(error, ERROR_BAD_FORMAT); return false; }

    std::unordered_map<std::wstring, Key> loaded;
    for (DWORD keyIndex = 0; keyIndex < keyCount; ++keyIndex)
    {
        std::wstring relative, displayRelative; DWORD valueCount = 0;
        if (!reader.String(&relative) || !reader.String(&displayRelative) || !reader.Scalar(&valueCount) || valueCount > MaxValues)
        { SetError(error, ERROR_BAD_FORMAT); return false; }
        const std::wstring path = relative.empty() ? hive.root : std::wstring(hive.root) + L"\\" + NormalizePath(relative);
        if (!IsPathOrChild(path, hive.root)) { SetError(error, ERROR_BAD_FORMAT); return false; }
        Key key; key.path = path;
        key.displayPath = displayRelative.empty() ? path : std::wstring(hive.root) + L"\\" + displayRelative;
        for (DWORD valueIndex = 0; valueIndex < valueCount; ++valueIndex)
        {
            Value value; DWORD dataBytes = 0;
            if (!reader.String(&value.name) || !reader.Scalar(&value.type) || !reader.Scalar(&dataBytes) || dataBytes > MaxValueBytes)
            { SetError(error, ERROR_BAD_FORMAT); return false; }
            value.data.resize(dataBytes);
            if (!reader.Bytes(value.data.data(), dataBytes)) { SetError(error, ERROR_BAD_FORMAT); return false; }
            key.values[Normalize(value.name.c_str())] = std::move(value);
        }
        loaded[path] = std::move(key);
    }
    if (!reader.End()) { SetError(error, ERROR_BAD_FORMAT); return false; }
    for (auto& entry : loaded) m_keys[entry.first] = std::move(entry.second);
    SetError(error, 0); return true;
}

bool GuestRegistryContext::SaveHiveLocked(const Hive& hive, DWORD* error) const
{
    std::vector<const Key*> keys;
    for (const auto& entry : m_keys) if (IsPathOrChild(entry.first, hive.root)) keys.push_back(&entry.second);
    std::sort(keys.begin(), keys.end(), [](const Key* a, const Key* b) { return a->path < b->path; });
    std::vector<BYTE> bytes(std::begin(Magic), std::end(Magic));
    Append<DWORD>(&bytes, FormatVersion); Append<DWORD>(&bytes, static_cast<DWORD>(keys.size()));
    const size_t rootLength = wcslen(hive.root);
    for (const Key* key : keys)
    {
        AppendString(&bytes, key->path == hive.root ? L"" : key->path.substr(rootLength + 1));
        AppendString(&bytes, key->displayPath.size() > rootLength ? key->displayPath.substr(rootLength + 1) : L"");
        std::vector<const Value*> values;
        for (const auto& entry : key->values) values.push_back(&entry.second);
        std::sort(values.begin(), values.end(), [](const Value* a, const Value* b) { return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0; });
        Append<DWORD>(&bytes, static_cast<DWORD>(values.size()));
        for (const Value* value : values)
        {
            AppendString(&bytes, value->name); Append<DWORD>(&bytes, value->type);
            Append<DWORD>(&bytes, static_cast<DWORD>(value->data.size()));
            bytes.insert(bytes.end(), value->data.begin(), value->data.end());
        }
    }
    if (bytes.size() > MaxHiveBytes) { SetError(error, ERROR_FILE_TOO_LARGE); return false; }
    const std::wstring temporary = std::wstring(hive.file) + L".wbrtmp";
    HANDLE file = INVALID_HANDLE_VALUE; DWORD storageError = 0;
    if (!m_storage->CreateFile(temporary.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
        nullptr, &file, &storageError)) { SetError(error, storageError); return false; }
    DWORD written = 0;
    const bool wrote = m_storage->WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, &storageError) && written == bytes.size();
    const bool flushed = wrote && m_storage->FlushFile(file, &storageError);
    DWORD ignored = 0; m_storage->CloseFile(file, &ignored);
    if (!wrote || !flushed)
    {
        m_storage->DeleteGuestFile(temporary.c_str(), &ignored);
        SetError(error, storageError ? storageError : ERROR_WRITE_FAULT); return false;
    }
    if (!m_storage->MoveGuestPath(temporary.c_str(), hive.file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, &storageError))
    {
        m_storage->DeleteGuestFile(temporary.c_str(), &ignored); SetError(error, storageError); return false;
    }
    SetError(error, 0); return true;
}

bool GuestRegistryContext::SavePathLocked(const std::wstring& path, DWORD* error) const
{
    const Hive* hive = HiveForPath(path);
    if (!hive) { SetError(error, 0); return true; }
    return SaveHiveLocked(*hive, error);
}
bool GuestRegistryContext::SaveAllLocked(DWORD* error) const
{
    for (const auto& hive : Hives) if (!SaveHiveLocked(hive, error)) return false;
    SetError(error, 0); return true;
}

bool GuestRegistryContext::EnsureInitializedLocked(DWORD* error)
{
    if (m_initialized) { SetError(error, 0); return true; }
    if (!m_storage) { SetError(error, ERROR_INVALID_FUNCTION); return false; }
    m_keys.clear(); bool missing = false;
    for (const auto& hive : Hives)
    {
        bool existed = false;
        if (!LoadHiveLocked(hive, &existed, error))
        {
            RuntimeDiagnostics::Record(L"REGISTRY: could not load logical hive " + std::wstring(hive.file) + L".");
            m_keys.clear(); return false;
        }
        missing = missing || !existed;
    }
    SeedDefaultsLocked(); m_initialized = true;
    if (missing && !SaveAllLocked(error)) { m_initialized = false; return false; }
    RuntimeDiagnostics::Record(L"REGISTRY: persistent logical hives loaded.");
    SetError(error, 0); return true;
}
bool GuestRegistryContext::Initialize(DWORD* error) { std::lock_guard<std::mutex> guard(m_lock); return EnsureInitializedLocked(error); }

bool GuestRegistryContext::OpenKey(HKEY parent, LPCWSTR subKey, REGSAM access, HKEY* result, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!result || !EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM parentAccess = 0;
    if (!ComposePathLocked(parent, subKey, &path, nullptr, &parentAccess, error)) return false;
    if (!IsPredefined(parent) && !HasAccess(parentAccess, KEY_ENUMERATE_SUB_KEYS)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    if (!m_keys.count(path)) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    const ULONG_PTR token = m_nextHandle++;
    m_handles[token] = { path, access ? access : KEY_READ }; *result = reinterpret_cast<HKEY>(token);
    SetError(error, 0); return true;
}

bool GuestRegistryContext::CreateKey(HKEY parent, LPCWSTR subKey, REGSAM access, HKEY* result, DWORD* disposition, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!result || !EnsureInitializedLocked(error)) return false;
    std::wstring path, display; REGSAM parentAccess = 0;
    if (!ComposePathLocked(parent, subKey, &path, &display, &parentAccess, error)) return false;
    if (!IsPredefined(parent) && !HasAccess(parentAccess, KEY_CREATE_SUB_KEY)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    if (!HiveForPath(path)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    const bool created = !m_keys.count(path);
    std::vector<std::wstring> createdPaths;
    size_t offset = 0;
    for (;;)
    {
        const size_t separator = path.find(L'\\', offset);
        const size_t length = separator == std::wstring::npos ? path.size() : separator;
        const std::wstring partial = path.substr(0, length);
        if (!m_keys.count(partial)) createdPaths.push_back(partial);
        if (separator == std::wstring::npos) break;
        offset = separator + 1;
    }
    EnsureKeyHierarchyLocked(path, display);
    if (created && !SavePathLocked(path, error))
    {
        for (const auto& createdPath : createdPaths) m_keys.erase(createdPath);
        return false;
    }
    if (disposition) *disposition = created ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
    const ULONG_PTR token = m_nextHandle++;
    m_handles[token] = { path, access ? access : KEY_READ | KEY_WRITE }; *result = reinterpret_cast<HKEY>(token);
    SetError(error, 0); return true;
}

bool GuestRegistryContext::QueryValue(HKEY handle, LPCWSTR name, LPDWORD type, LPBYTE data, LPDWORD bytes, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!bytes) { SetError(error, ERROR_INVALID_PARAMETER); return false; }
    if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0;
    if (!ResolveLocked(handle, &path, &access, error)) return false;
    if (!HasAccess(access, KEY_QUERY_VALUE)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    const auto key = m_keys.find(path);
    if (key == m_keys.end()) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    const auto value = key->second.values.find(Normalize(name));
    if (value == key->second.values.end()) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    if (type) *type = value->second.type;
    const DWORD required = static_cast<DWORD>(value->second.data.size());
    if (!data) { *bytes = required; SetError(error, 0); return true; }
    if (*bytes < required) { *bytes = required; SetError(error, ERROR_MORE_DATA); return false; }
    if (required) memcpy(data, value->second.data.data(), required);
    *bytes = required; SetError(error, 0); return true;
}

bool GuestRegistryContext::SetValue(HKEY handle, LPCWSTR name, DWORD type, const BYTE* data, DWORD bytes, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if ((bytes && !data) || bytes > MaxValueBytes) { SetError(error, ERROR_INVALID_PARAMETER); return false; }
    if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0;
    if (!ResolveLocked(handle, &path, &access, error)) return false;
    if (!HasAccess(access, KEY_SET_VALUE)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    auto key = m_keys.find(path); if (key == m_keys.end()) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    if (!HiveForPath(path)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    const std::wstring normalizedName = Normalize(name);
    const auto previous = key->second.values.find(normalizedName);
    const bool hadPrevious = previous != key->second.values.end();
    Value previousValue;
    if (hadPrevious) previousValue = previous->second;
    Value value; value.name = name ? name : L""; value.type = type;
    if (bytes) value.data.assign(data, data + bytes);
    key->second.values[normalizedName] = std::move(value);
    if (!SavePathLocked(path, error))
    {
        if (hadPrevious) key->second.values[normalizedName] = std::move(previousValue);
        else key->second.values.erase(normalizedName);
        return false;
    }
    SetError(error, 0); return true;
}

bool GuestRegistryContext::DeleteValue(HKEY handle, LPCWSTR name, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock); if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0; if (!ResolveLocked(handle, &path, &access, error)) return false;
    if (!HasAccess(access, KEY_SET_VALUE)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    auto key = m_keys.find(path); const std::wstring normalized = Normalize(name);
    if (!HiveForPath(path)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    if (key == m_keys.end() || !key->second.values.count(normalized)) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    Value previous = key->second.values[normalized];
    key->second.values.erase(normalized);
    if (!SavePathLocked(path, error))
    {
        key->second.values[normalized] = std::move(previous);
        return false;
    }
    SetError(error, 0); return true;
}

bool GuestRegistryContext::DeleteKey(HKEY parent, LPCWSTR subKey, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock); if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0; if (!ComposePathLocked(parent, subKey, &path, nullptr, &access, error)) return false;
    if (!IsPredefined(parent) && !HasAccess(access, KEY_CREATE_SUB_KEY)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    const Hive* hive = HiveForPath(path);
    if (!hive || path == hive->root) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    if (!m_keys.count(path)) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    for (const auto& entry : m_keys) if (entry.first != path && IsPathOrChild(entry.first, path))
    { SetError(error, ERROR_ACCESS_DENIED); return false; }
    Key previous = m_keys[path];
    m_keys.erase(path);
    if (!SavePathLocked(path, error))
    {
        m_keys[path] = std::move(previous);
        return false;
    }
    SetError(error, 0); return true;
}

bool GuestRegistryContext::EnumKey(HKEY handle, DWORD index, LPWSTR name, LPDWORD characters, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!characters) { SetError(error, ERROR_INVALID_PARAMETER); return false; }
    if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0; if (!ResolveLocked(handle, &path, &access, error)) return false;
    if (!HasAccess(access, KEY_ENUMERATE_SUB_KEYS)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    std::vector<std::wstring> names; const std::wstring prefix = path + L"\\";
    for (const auto& entry : m_keys)
    {
        if (entry.first.compare(0, prefix.size(), prefix)) continue;
        const std::wstring remainder = entry.first.substr(prefix.size());
        if (remainder.empty() || remainder.find(L'\\') != std::wstring::npos) continue;
        const size_t separator = entry.second.displayPath.find_last_of(L'\\');
        names.push_back(separator == std::wstring::npos ? remainder : entry.second.displayPath.substr(separator + 1));
    }
    if (path == L"hku") names.push_back(L"Win32Bridge");
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    if (index >= names.size()) { SetError(error, ERROR_NO_MORE_ITEMS); return false; }
    const DWORD required = static_cast<DWORD>(names[index].size());
    if (!name || *characters <= required) { *characters = required; SetError(error, ERROR_MORE_DATA); return false; }
    memcpy(name, names[index].c_str(), (required + 1) * sizeof(wchar_t)); *characters = required;
    SetError(error, 0); return true;
}

bool GuestRegistryContext::EnumValue(HKEY handle, DWORD index, LPWSTR name, LPDWORD characters,
    LPDWORD type, LPBYTE data, LPDWORD bytes, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!characters) { SetError(error, ERROR_INVALID_PARAMETER); return false; }
    if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0; if (!ResolveLocked(handle, &path, &access, error)) return false;
    if (!HasAccess(access, KEY_QUERY_VALUE)) { SetError(error, ERROR_ACCESS_DENIED); return false; }
    const auto key = m_keys.find(path); if (key == m_keys.end()) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    std::vector<const Value*> values; for (const auto& entry : key->second.values) values.push_back(&entry.second);
    std::sort(values.begin(), values.end(), [](const Value* a, const Value* b) { return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0; });
    if (index >= values.size()) { SetError(error, ERROR_NO_MORE_ITEMS); return false; }
    const Value& value = *values[index]; const DWORD nameSize = static_cast<DWORD>(value.name.size());
    const DWORD dataSize = static_cast<DWORD>(value.data.size());
    if (type) *type = value.type;
    const DWORD suppliedDataBytes = bytes ? *bytes : 0;
    if (bytes) *bytes = dataSize;
    if (!name || *characters <= nameSize || (data && (!bytes || suppliedDataBytes < dataSize)))
    { *characters = nameSize; SetError(error, ERROR_MORE_DATA); return false; }
    memcpy(name, value.name.c_str(), (nameSize + 1) * sizeof(wchar_t)); *characters = nameSize;
    if (data && dataSize) memcpy(data, value.data.data(), dataSize);
    SetError(error, 0); return true;
}

bool GuestRegistryContext::QueryInfoKey(HKEY handle, LPDWORD subKeyCount, LPDWORD maximumSubKeyLength,
    LPDWORD valueCount, LPDWORD maximumValueNameLength, LPDWORD maximumValueDataLength,
    PFILETIME lastWriteTime, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock); if (!EnsureInitializedLocked(error)) return false;
    std::wstring path; REGSAM access = 0; if (!ResolveLocked(handle, &path, &access, error)) return false;
    const auto key = m_keys.find(path); if (key == m_keys.end()) { SetError(error, ERROR_FILE_NOT_FOUND); return false; }
    DWORD children = 0, longestChild = 0; const std::wstring prefix = path + L"\\";
    for (const auto& entry : m_keys)
    {
        if (entry.first.compare(0, prefix.size(), prefix)) continue;
        const std::wstring rest = entry.first.substr(prefix.size());
        if (!rest.empty() && rest.find(L'\\') == std::wstring::npos) { ++children; longestChild = (std::max)(longestChild, static_cast<DWORD>(rest.size())); }
    }
    DWORD longestName = 0, longestData = 0;
    for (const auto& entry : key->second.values)
    {
        longestName = (std::max)(longestName, static_cast<DWORD>(entry.second.name.size()));
        longestData = (std::max)(longestData, static_cast<DWORD>(entry.second.data.size()));
    }
    if (subKeyCount) *subKeyCount = children; if (maximumSubKeyLength) *maximumSubKeyLength = longestChild;
    if (valueCount) *valueCount = static_cast<DWORD>(key->second.values.size());
    if (maximumValueNameLength) *maximumValueNameLength = longestName;
    if (maximumValueDataLength) *maximumValueDataLength = longestData;
    if (lastWriteTime) ZeroMemory(lastWriteTime, sizeof(*lastWriteTime));
    SetError(error, 0); return true;
}

bool GuestRegistryContext::Flush(DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!m_initialized) { SetError(error, 0); return true; }
    return SaveAllLocked(error);
}
bool GuestRegistryContext::CloseKey(HKEY handle, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_lock); const auto found = m_handles.find(reinterpret_cast<ULONG_PTR>(handle));
    if (found == m_handles.end()) { SetError(error, ERROR_INVALID_HANDLE); return false; }
    m_handles.erase(found); SetError(error, 0); return true;
}

GuestRegistryContext* Win32Bridge::Bridge::CurrentGuestRegistryContext() { return g_registry; }
GuestRegistryScope::GuestRegistryScope(GuestRegistryContext* value) : m_previous(g_registry) { g_registry = value; }
GuestRegistryScope::~GuestRegistryScope() { g_registry = m_previous; }
