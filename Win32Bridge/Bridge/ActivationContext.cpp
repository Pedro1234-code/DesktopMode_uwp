#include "pch.h"
#include "Bridge/ActivationContext.h"

#include "Bridge/GuestResources.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr DWORD kActCtxProcessorArchitectureValid = 0x00000001;
    constexpr DWORD kActCtxLangIdValid = 0x00000002;
    constexpr DWORD kActCtxAssemblyDirectoryValid = 0x00000004;
    constexpr DWORD kActCtxResourceNameValid = 0x00000008;
    constexpr DWORD kActCtxSetProcessDefault = 0x00000010;
    constexpr DWORD kActCtxApplicationNameValid = 0x00000020;
    constexpr DWORD kActCtxModuleValid = 0x00000080;
    constexpr DWORD kDeactivateActCtxForceEarly = 0x00000001;
    constexpr DWORD kQueryActCtxUseActive = 0x00000004;
    constexpr DWORD kQueryActCtxNoAddRef = 0x80000000;

    struct GuestActivationContextBasicInformation final
    {
        HANDLE hActCtx;
        DWORD flags;
    };

    struct ContextRecord final
    {
        GuestManifestInfo manifest;
        std::wstring resourceName;
        std::atomic<ULONG> references{ 1 };
    };

    struct ActiveRecord final
    {
        HANDLE context = nullptr;
        ULONG_PTR cookie = 0;
    };

    constexpr ULONG_PTR FirstContextToken = 0x74000000;
    std::atomic<ULONG_PTR> g_nextContext{ FirstContextToken };
    std::atomic<ULONG_PTR> g_nextCookie{ 1 };
    std::mutex g_contextLock;
    std::unordered_map<ULONG_PTR, std::shared_ptr<ContextRecord>> g_contexts;
    std::atomic<ULONG_PTR> g_processContext{ 0 };
    thread_local std::vector<ActiveRecord> g_activeContexts;

    std::wstring Lower(std::wstring value)
    {
        for (auto& character : value) character = static_cast<wchar_t>(towlower(character));
        return value;
    }

    std::wstring Trim(std::wstring value)
    {
        const auto first = std::find_if(value.begin(), value.end(), [](wchar_t c) { return !iswspace(c); });
        const auto last = std::find_if(value.rbegin(), value.rend(), [](wchar_t c) { return !iswspace(c); }).base();
        if (first >= last) return {};
        return std::wstring(first, last);
    }

    std::wstring DirectoryOf(const std::wstring& path)
    {
        const size_t separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
    }

    std::wstring JoinPath(const std::wstring& directory, const std::wstring& leaf)
    {
        if (directory.empty()) return leaf;
        if (leaf.empty()) return directory;
        return directory.back() == L'\\' || directory.back() == L'/'
            ? directory + leaf : directory + L"\\" + leaf;
    }

    bool DecodeXml(const BYTE* bytes, size_t size, std::wstring* xml)
    {
        if (!bytes || !xml || size == 0) return false;
        xml->clear();
        if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe)
        {
            const size_t count = (size - 2) / sizeof(wchar_t);
            xml->assign(reinterpret_cast<const wchar_t*>(bytes + 2), count);
            return true;
        }
        if (size >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff)
        {
            for (size_t offset = 2; offset + 1 < size; offset += 2)
                xml->push_back(static_cast<wchar_t>((bytes[offset] << 8) | bytes[offset + 1]));
            return true;
        }
        // Resource compilers may emit UTF-16 XML without a BOM. Detect the
        // characteristic ASCII/XML prefix instead of treating it as ANSI.
        if (size >= 4 && bytes[0] == L'<' && bytes[1] == 0 && bytes[2] != 0 && bytes[3] == 0)
        {
            for (size_t offset = 0; offset + 1 < size; offset += 2)
            {
                WORD character = 0;
                memcpy(&character, bytes + offset, sizeof(character));
                if (character == 0) break;
                xml->push_back(static_cast<wchar_t>(character));
            }
            return !xml->empty();
        }
        const BYTE* source = bytes;
        int sourceSize = static_cast<int>((std::min)(size, static_cast<size_t>((std::numeric_limits<int>::max)())));
        if (sourceSize >= 3 && source[0] == 0xef && source[1] == 0xbb && source[2] == 0xbf)
        {
            source += 3;
            sourceSize -= 3;
        }
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<LPCCH>(source), sourceSize, nullptr, 0);
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (count <= 0)
        {
            codePage = CP_ACP;
            flags = 0;
            count = MultiByteToWideChar(codePage, flags,
                reinterpret_cast<LPCCH>(source), sourceSize, nullptr, 0);
        }
        if (count <= 0) return false;
        xml->resize(static_cast<size_t>(count));
        return MultiByteToWideChar(codePage, flags, reinterpret_cast<LPCCH>(source), sourceSize,
            &(*xml)[0], count) == count;
    }

    bool FindElementRange(
        const std::wstring& lower,
        const std::wstring& localName,
        size_t searchFrom,
        size_t* elementStart,
        size_t* elementEnd)
    {
        size_t cursor = searchFrom;
        while ((cursor = lower.find(L'<', cursor)) != std::wstring::npos)
        {
            size_t nameStart = cursor + 1;
            if (nameStart >= lower.size()) return false;
            if (lower[nameStart] == L'/' || lower[nameStart] == L'!' || lower[nameStart] == L'?')
            {
                ++cursor;
                continue;
            }
            const size_t close = lower.find(L'>', nameStart);
            if (close == std::wstring::npos) return false;
            size_t nameEnd = nameStart;
            while (nameEnd < close && !iswspace(lower[nameEnd]) && lower[nameEnd] != L'/') ++nameEnd;
            const size_t colon = lower.rfind(L':', nameEnd);
            const size_t localStart = colon != std::wstring::npos && colon >= nameStart ? colon + 1 : nameStart;
            if (lower.compare(localStart, nameEnd - localStart, localName) == 0)
            {
                *elementStart = cursor;
                *elementEnd = close;
                return true;
            }
            cursor = close + 1;
        }
        return false;
    }

    std::wstring Attribute(const std::wstring& original, const std::wstring& lower,
        const std::wstring& element, const std::wstring& attribute)
    {
        size_t elementStart = 0;
        size_t elementEnd = 0;
        if (!FindElementRange(lower, element, 0, &elementStart, &elementEnd)) return {};
        size_t attributeStart = lower.find(attribute, elementStart + 1);
        if (attributeStart == std::wstring::npos || attributeStart >= elementEnd) return {};
        attributeStart += attribute.size();
        while (attributeStart < elementEnd && iswspace(lower[attributeStart])) ++attributeStart;
        if (attributeStart >= elementEnd || lower[attributeStart++] != L'=') return {};
        while (attributeStart < elementEnd && iswspace(lower[attributeStart])) ++attributeStart;
        if (attributeStart >= elementEnd || (lower[attributeStart] != L'\'' && lower[attributeStart] != L'"')) return {};
        const wchar_t quote = lower[attributeStart++];
        const size_t end = lower.find(quote, attributeStart);
        if (end == std::wstring::npos || end > elementEnd) return {};
        return original.substr(attributeStart, end - attributeStart);
    }

    GuestAssemblyIdentity IdentityFromTag(const std::wstring& original, const std::wstring& lower)
    {
        GuestAssemblyIdentity identity;
        identity.name = Attribute(original, lower, L"assemblyidentity", L"name");
        identity.version = Attribute(original, lower, L"assemblyidentity", L"version");
        identity.processorArchitecture = Attribute(original, lower, L"assemblyidentity", L"processorarchitecture");
        identity.publicKeyToken = Attribute(original, lower, L"assemblyidentity", L"publickeytoken");
        identity.language = Attribute(original, lower, L"assemblyidentity", L"language");
        identity.type = Attribute(original, lower, L"assemblyidentity", L"type");
        return identity;
    }

    bool IdentityMatches(const GuestAssemblyIdentity& requested, const GuestAssemblyIdentity& candidate)
    {
        const auto equal = [](const std::wstring& left, const std::wstring& right)
        { return left.empty() || left == L"*" || _wcsicmp(left.c_str(), right.c_str()) == 0; };
        return !requested.name.empty() && _wcsicmp(requested.name.c_str(), candidate.name.c_str()) == 0 &&
            equal(requested.version, candidate.version) &&
            equal(requested.processorArchitecture, candidate.processorArchitecture) &&
            equal(requested.publicKeyToken, candidate.publicKeyToken) &&
            equal(requested.language, candidate.language) && equal(requested.type, candidate.type);
    }

    std::wstring ElementText(const std::wstring& original, const std::wstring& lower, const std::wstring& localName)
    {
        size_t start = 0;
        while ((start = lower.find(L'<' , start)) != std::wstring::npos)
        {
            const size_t nameStart = start + 1;
            const size_t close = lower.find(L'>', nameStart);
            if (close == std::wstring::npos) return {};
            size_t qualifiedEnd = nameStart;
            while (qualifiedEnd < close && !iswspace(lower[qualifiedEnd]) &&
                lower[qualifiedEnd] != L'/' && lower[qualifiedEnd] != L'>') ++qualifiedEnd;
            const size_t colon = lower.rfind(L':', qualifiedEnd);
            const size_t actualStart = colon != std::wstring::npos && colon >= nameStart ? colon + 1 : nameStart;
            const size_t actualEnd = qualifiedEnd;
            if (lower.compare(actualStart, actualEnd - actualStart, localName) == 0)
            {
                const std::wstring closing = L"</" + lower.substr(nameStart, actualEnd - nameStart) + L">";
                const size_t end = lower.find(closing, close + 1);
                if (end != std::wstring::npos) return Trim(original.substr(close + 1, end - close - 1));
            }
            start = close + 1;
        }
        return {};
    }

    bool ParseGuid(const std::wstring& value, GUID* guid)
    {
        if (!guid) return false;
        unsigned long d1 = 0, d2 = 0, d3 = 0;
        unsigned int d[8]{};
        const int count = swscanf_s(value.c_str(), L"{%8lx-%4lx-%4lx-%2x%2x-%2x%2x%2x%2x%2x%2x}",
            &d1, &d2, &d3, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
        const int bareCount = count == 11 ? count : swscanf_s(value.c_str(), L"%8lx-%4lx-%4lx-%2x%2x-%2x%2x%2x%2x%2x%2x",
            &d1, &d2, &d3, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
        if (bareCount != 11) return false;
        guid->Data1 = d1;
        guid->Data2 = static_cast<WORD>(d2);
        guid->Data3 = static_cast<WORD>(d3);
        for (size_t i = 0; i < 8; ++i) guid->Data4[i] = static_cast<BYTE>(d[i]);
        return true;
    }

    bool ParseManifest(const BYTE* bytes, size_t size, const std::wstring& source, GuestManifestInfo* info)
    {
        if (!info || !DecodeXml(bytes, size, &info->xml)) return false;
        info->source = source;
        const std::wstring lower = Lower(info->xml);
        if (lower.find(L"<assembly") == std::wstring::npos) return false;

        size_t identitySearch = 0;
        size_t identityStart = 0;
        size_t identityEnd = 0;
        bool rootIdentity = true;
        while (FindElementRange(lower, L"assemblyidentity", identitySearch, &identityStart, &identityEnd))
        {
            const std::wstring tagOriginal = info->xml.substr(identityStart, identityEnd - identityStart + 1);
            const std::wstring tagLower = lower.substr(identityStart, identityEnd - identityStart + 1);
            const GuestAssemblyIdentity identity = IdentityFromTag(tagOriginal, tagLower);
            const std::wstring name = Lower(identity.name);
            const std::wstring version = Lower(identity.version);
            if (rootIdentity)
            {
                info->identity = identity;
                info->assemblyIdentity = identity.name;
                if (!identity.version.empty()) info->assemblyIdentity += L",version=" + identity.version;
                rootIdentity = false;
            }
            else
            {
                info->dependencies.push_back(identity);
            }
            if (name == L"microsoft.windows.common-controls" && version.rfind(L"6.", 0) == 0)
            {
                info->commonControlsV6 = true;
            }
            identitySearch = identityEnd + 1;
        }
        info->dpiAwareness = ElementText(info->xml, lower, L"dpiawareness");
        if (info->dpiAwareness.empty()) info->dpiAwareness = ElementText(info->xml, lower, L"dpiaware");
        const std::wstring longPath = Lower(ElementText(info->xml, lower, L"longpathaware"));
        info->longPathAware = longPath == L"true" || longPath == L"1";

        const std::wstring level = Lower(Attribute(info->xml, lower, L"requestedexecutionlevel", L"level"));
        if (level == L"asinvoker") info->runLevel = ACTCTX_RUN_LEVEL_AS_INVOKER;
        else if (level == L"highestavailable") info->runLevel = ACTCTX_RUN_LEVEL_HIGHEST_AVAILABLE;
        else if (level == L"requireadministrator") info->runLevel = ACTCTX_RUN_LEVEL_REQUIRE_ADMIN;
        const std::wstring uiAccess = Lower(Attribute(info->xml, lower, L"requestedexecutionlevel", L"uiaccess"));
        info->uiAccess = uiAccess == L"true" || uiAccess == L"1";

        size_t cursor = 0;
        size_t supportedStart = 0;
        size_t supportedEnd = 0;
        while (FindElementRange(lower, L"supportedos", cursor, &supportedStart, &supportedEnd))
        {
            const std::wstring sliceOriginal = info->xml.substr(supportedStart, supportedEnd - supportedStart + 1);
            const std::wstring sliceLower = lower.substr(supportedStart, supportedEnd - supportedStart + 1);
            GUID id{};
            const std::wstring value = Attribute(sliceOriginal, sliceLower, L"supportedos", L"id");
            if (ParseGuid(value, &id)) info->supportedOperatingSystems.push_back(id);
            cursor = supportedEnd + 1;
        }
        return true;
    }

    void ParseAssemblyFiles(const GuestManifestInfo& manifest, const std::wstring& directory,
        GuestAssemblyInfo* assembly)
    {
        if (!assembly) return;
        const std::wstring lower = Lower(manifest.xml);
        size_t cursor = 0, start = 0, end = 0;
        while (FindElementRange(lower, L"file", cursor, &start, &end))
        {
            const std::wstring tag = manifest.xml.substr(start, end - start + 1);
            const std::wstring tagLower = lower.substr(start, end - start + 1);
            const std::wstring name = Attribute(tag, tagLower, L"file", L"name");
            if (!name.empty()) assembly->files.push_back({ name, JoinPath(directory, name) });
            cursor = end + 1;
        }
    }

    bool ReadManifestFile(GuestStorageContext* storage, const std::wstring& path, GuestManifestInfo* manifest)
    {
        std::vector<BYTE> bytes;
        DWORD error = ERROR_SUCCESS;
        return storage && storage->ReadAllBytes(path.c_str(), &bytes, &error) &&
            ParseManifest(bytes.data(), bytes.size(), path, manifest);
    }

    bool FindAssemblyManifest(GuestStorageContext* storage, const GuestAssemblyIdentity& identity,
        const std::wstring& applicationDirectory, GuestManifestInfo* manifest)
    {
        const std::wstring localName = identity.name + L".manifest";
        const std::wstring localCandidates[] = {
            JoinPath(applicationDirectory, localName),
            JoinPath(JoinPath(applicationDirectory, identity.name), localName)
        };
        for (const auto& candidate : localCandidates)
        {
            GuestManifestInfo parsed;
            if (ReadManifestFile(storage, candidate, &parsed) && IdentityMatches(identity, parsed.identity))
            {
                *manifest = std::move(parsed);
                return true;
            }
        }

        WIN32_FIND_DATAW data{};
        HANDLE find = INVALID_HANDLE_VALUE;
        DWORD error = ERROR_SUCCESS;
        if (!storage->FindFirstGuestFile(L"C:\\Windows\\WinSxS\\Manifests\\*.manifest", &data, &find, &error))
            return false;
        bool found = false;
        do
        {
            GuestManifestInfo parsed;
            const std::wstring candidate = JoinPath(L"C:\\Windows\\WinSxS\\Manifests", data.cFileName);
            if (ReadManifestFile(storage, candidate, &parsed) && IdentityMatches(identity, parsed.identity))
            {
                *manifest = std::move(parsed);
                found = true;
                break;
            }
        } while (storage->FindNextGuestFile(find, &data, &error));
        storage->CloseFindHandle(find, nullptr);
        return found;
    }

    void ResolveAssemblies(ContextRecord* record)
    {
        if (!record) return;
        GuestStorageContext* storage = CurrentGuestStorageContext();
        if (!storage) return;
        const std::wstring applicationDirectory = record->manifest.assemblyDirectory.empty()
            ? DirectoryOf(record->manifest.source) : record->manifest.assemblyDirectory;
        std::unordered_set<std::wstring> visited;
        std::vector<GuestAssemblyIdentity> pending = record->manifest.dependencies;
        for (size_t index = 0; index < pending.size(); ++index)
        {
            const GuestAssemblyIdentity dependency = pending[index];
            const std::wstring key = Lower(dependency.name + L"," + dependency.version + L"," +
                dependency.processorArchitecture + L"," + dependency.publicKeyToken + L"," + dependency.language);
            if (dependency.name.empty() || !visited.emplace(key).second) continue;

            GuestAssemblyInfo assembly;
            assembly.identity = dependency;
            GuestManifestInfo dependentManifest;
            if (FindAssemblyManifest(storage, dependency, applicationDirectory, &dependentManifest))
            {
                assembly.identity = dependentManifest.identity;
                assembly.manifestPath = dependentManifest.source;
                assembly.directory = DirectoryOf(dependentManifest.source);
                if (Lower(assembly.directory) == Lower(L"C:\\Windows\\WinSxS\\Manifests"))
                {
                    std::wstring leaf = dependentManifest.source.substr(assembly.directory.size() + 1);
                    const size_t suffix = Lower(leaf).rfind(L".manifest");
                    if (suffix != std::wstring::npos) leaf.erase(suffix);
                    assembly.directory = JoinPath(L"C:\\Windows\\WinSxS", leaf);
                }
                ParseAssemblyFiles(dependentManifest, assembly.directory, &assembly);
                pending.insert(pending.end(), dependentManifest.dependencies.begin(), dependentManifest.dependencies.end());
            }
            else if (_wcsicmp(dependency.name.c_str(), L"Microsoft.Windows.Common-Controls") == 0 &&
                dependency.version.rfind(L"6.", 0) == 0)
            {
                assembly.virtualAssembly = true;
                assembly.directory = L"C:\\Windows\\WinSxS\\Virtual\\Microsoft.Windows.Common-Controls";
                assembly.files.push_back({ L"comctl32.dll", L"C:\\Windows\\System32\\comctl32.dll" });
            }
            else
            {
                RuntimeDiagnostics::Record(L"SXS: unresolved assembly " + dependency.name + L" " + dependency.version + L".");
                continue;
            }
            record->manifest.assemblies.push_back(std::move(assembly));
        }
    }

    std::shared_ptr<ContextRecord> Lookup(HANDLE handle)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE) return {};
        std::lock_guard<std::mutex> guard(g_contextLock);
        const auto found = g_contexts.find(reinterpret_cast<ULONG_PTR>(handle));
        return found == g_contexts.end() ? std::shared_ptr<ContextRecord>{} : found->second;
    }

    HANDLE Store(std::shared_ptr<ContextRecord> record)
    {
        if (!record) return INVALID_HANDLE_VALUE;
        ResolveAssemblies(record.get());
        std::lock_guard<std::mutex> guard(g_contextLock);
        ULONG_PTR token = g_nextContext.fetch_add(1);
        while (token == 0 || token == reinterpret_cast<ULONG_PTR>(INVALID_HANDLE_VALUE) || g_contexts.count(token))
            token = g_nextContext.fetch_add(1);
        g_contexts.emplace(token, std::move(record));
        return reinterpret_cast<HANDLE>(token);
    }

    bool LoadManifestResource(HMODULE module, LPCWSTR name, LANGID language, bool exact,
        const std::wstring& source, std::shared_ptr<ContextRecord>* output)
    {
        GuestResourceData resource;
        if (FindGuestResource(module, MAKEINTRESOURCEW(24), name, language, exact, &resource) !=
            GuestResourceStatus::Success)
            return false;
        auto record = std::make_shared<ContextRecord>();
        if (!ParseManifest(resource.data, resource.size, source, &record->manifest)) return false;
        if (reinterpret_cast<ULONG_PTR>(name) <= 0xffff)
            record->resourceName = L"#" + std::to_wstring(reinterpret_cast<ULONG_PTR>(name));
        else
            record->resourceName = name;
        *output = std::move(record);
        return true;
    }

    bool ConvertAnsi(LPCSTR source, std::wstring* destination)
    {
        if (!source) return false;
        const int required = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1, nullptr, 0);
        if (required <= 0) return false;
        destination->resize(static_cast<size_t>(required));
        if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1,
            &(*destination)[0], required) != required) return false;
        destination->resize(static_cast<size_t>(required - 1));
        return true;
    }

    HANDLE CurrentHandle()
    {
        return g_activeContexts.empty()
            ? reinterpret_cast<HANDLE>(g_processContext.load()) : g_activeContexts.back().context;
    }

    template<typename T>
    BOOL ReturnFixed(const T& information, PVOID buffer, SIZE_T size, SIZE_T* required)
    {
        if (required) *required = sizeof(T);
        if (!buffer || size < sizeof(T))
        {
            BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(buffer, &information, sizeof(T));
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
}

const GuestManifestInfo* Win32Bridge::Bridge::CurrentGuestManifest()
{
    const auto record = Lookup(CurrentHandle());
    return record ? &record->manifest : nullptr;
}

bool Win32Bridge::Bridge::CurrentGuestUsesVisualStyles()
{
    const GuestManifestInfo* manifest = CurrentGuestManifest();
    return manifest && manifest->commonControlsV6;
}

bool Win32Bridge::Bridge::ResolveGuestActivationContextModule(LPCWSTR moduleName, std::wstring* path)
{
    if (!moduleName || !path) return false;
    std::wstring requested = moduleName;
    const size_t separator = requested.find_last_of(L"\\/");
    if (separator != std::wstring::npos) requested.erase(0, separator + 1);
    const GuestManifestInfo* manifest = CurrentGuestManifest();
    if (!manifest) return false;
    for (auto assembly = manifest->assemblies.rbegin(); assembly != manifest->assemblies.rend(); ++assembly)
    {
        for (const auto& file : assembly->files)
        {
            std::wstring name = file.name;
            const size_t fileSeparator = name.find_last_of(L"\\/");
            if (fileSeparator != std::wstring::npos) name.erase(0, fileSeparator + 1);
            if (_wcsicmp(name.c_str(), requested.c_str()) == 0)
            {
                *path = file.sourcePath;
                RuntimeDiagnostics::Record(L"SXS: " + requested + L" -> " + *path + L".");
                return true;
            }
        }
    }
    return false;
}

GuestActivationContextScope::GuestActivationContextScope()
    : m_previous(reinterpret_cast<HANDLE>(g_processContext.load()))
{
    std::vector<GuestResourceIdentifier> names;
    if (EnumerateGuestResourceNames(nullptr, MAKEINTRESOURCEW(24), &names) != GuestResourceStatus::Success || names.empty())
        return;
    const GuestResourceIdentifier& name = names.front();
    const LPCWSTR resourceName = name.ordinal ? MAKEINTRESOURCEW(name.id) : name.text.c_str();
    std::shared_ptr<ContextRecord> record;
    GuestStorageContext* storage = CurrentGuestStorageContext();
    const std::wstring source = storage ? storage->ModulePath() : L"<embedded process manifest>";
    if (!LoadManifestResource(nullptr, resourceName, 0, false, source, &record)) return;
    m_context = Store(std::move(record));
    g_processContext.store(reinterpret_cast<ULONG_PTR>(m_context));
    RuntimeDiagnostics::Record(L"MANIFEST: activated the embedded process manifest.");
}

GuestActivationContextScope::~GuestActivationContextScope()
{
    for (const ActiveRecord& active : g_activeContexts)
        BridgeReleaseActCtx(active.context);
    g_activeContexts.clear();
    g_processContext.store(reinterpret_cast<ULONG_PTR>(m_previous));
    if (m_context && m_context != INVALID_HANDLE_VALUE) BridgeReleaseActCtx(m_context);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateActCtxW(const GuestActCtxW* context)
{
    if (!context || context->cbSize != sizeof(GuestActCtxW) || !context->lpSource)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const DWORD supported = kActCtxProcessorArchitectureValid | kActCtxLangIdValid |
        kActCtxAssemblyDirectoryValid | kActCtxResourceNameValid |
        kActCtxSetProcessDefault | kActCtxApplicationNameValid | kActCtxModuleValid;
    if ((context->dwFlags & ~supported) != 0)
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE_VALUE;
    }

    std::shared_ptr<ContextRecord> record;
    const LANGID language = (context->dwFlags & kActCtxLangIdValid) ? context->wLangId : 0;
    if ((context->dwFlags & kActCtxResourceNameValid) != 0)
    {
        const HMODULE module = (context->dwFlags & kActCtxModuleValid) ? context->hModule : nullptr;
        if (!LoadManifestResource(module, context->lpResourceName, language,
            (context->dwFlags & kActCtxLangIdValid) != 0, context->lpSource, &record))
        {
            GuestStorageContext* storage = CurrentGuestStorageContext();
            std::vector<BYTE> file;
            std::vector<BYTE> manifest;
            DWORD error = ERROR_SUCCESS;
            if (!storage || !storage->ReadAllBytes(context->lpSource, &file, &error) ||
                CopyGuestFileResource(file.data(), file.size(), MAKEINTRESOURCEW(24), context->lpResourceName,
                    language, (context->dwFlags & kActCtxLangIdValid) != 0, &manifest) != GuestResourceStatus::Success)
            {
                BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_RESOURCE_DATA_NOT_FOUND : error);
                return INVALID_HANDLE_VALUE;
            }
            record = std::make_shared<ContextRecord>();
            if (!ParseManifest(manifest.data(), manifest.size(), context->lpSource, &record->manifest))
            {
                BridgeSetLastError(ERROR_SXS_CANT_GEN_ACTCTX);
                return INVALID_HANDLE_VALUE;
            }
        }
    }
    else
    {
        GuestStorageContext* storage = CurrentGuestStorageContext();
        std::vector<BYTE> manifest;
        DWORD error = ERROR_SUCCESS;
        if (!storage || !storage->ReadAllBytes(context->lpSource, &manifest, &error))
        {
            BridgeSetLastError(error);
            return INVALID_HANDLE_VALUE;
        }
        record = std::make_shared<ContextRecord>();
        if (!ParseManifest(manifest.data(), manifest.size(), context->lpSource, &record->manifest))
        {
            BridgeSetLastError(ERROR_SXS_CANT_GEN_ACTCTX);
            return INVALID_HANDLE_VALUE;
        }
    }
    if ((context->dwFlags & kActCtxAssemblyDirectoryValid) != 0 && context->lpAssemblyDirectory)
        record->manifest.assemblyDirectory = context->lpAssemblyDirectory;
    const HANDLE handle = Store(std::move(record));
    if ((context->dwFlags & kActCtxSetProcessDefault) != 0)
        g_processContext.store(reinterpret_cast<ULONG_PTR>(handle));
    BridgeSetLastError(ERROR_SUCCESS);
    return handle;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateActCtxA(const GuestActCtxA* context)
{
    if (!context || context->cbSize != sizeof(GuestActCtxA))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::wstring source, directory, resource, application;
    if (!ConvertAnsi(context->lpSource, &source) ||
        ((context->dwFlags & kActCtxAssemblyDirectoryValid) && !ConvertAnsi(context->lpAssemblyDirectory, &directory)) ||
        ((context->dwFlags & kActCtxApplicationNameValid) && !ConvertAnsi(context->lpApplicationName, &application)))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return INVALID_HANDLE_VALUE;
    }
    LPCWSTR resourceName = nullptr;
    if ((context->dwFlags & kActCtxResourceNameValid) != 0)
    {
        if (reinterpret_cast<ULONG_PTR>(context->lpResourceName) <= 0xffff)
            resourceName = reinterpret_cast<LPCWSTR>(context->lpResourceName);
        else if (!ConvertAnsi(context->lpResourceName, &resource))
        {
            BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
            return INVALID_HANDLE_VALUE;
        }
        else resourceName = resource.c_str();
    }
    GuestActCtxW wide{};
    wide.cbSize = sizeof(wide);
    wide.dwFlags = context->dwFlags;
    wide.lpSource = source.c_str();
    wide.wProcessorArchitecture = context->wProcessorArchitecture;
    wide.wLangId = context->wLangId;
    wide.lpAssemblyDirectory = directory.empty() ? nullptr : directory.c_str();
    wide.lpResourceName = resourceName;
    wide.lpApplicationName = application.empty() ? nullptr : application.c_str();
    wide.hModule = context->hModule;
    return BridgeCreateActCtxW(&wide);
}

void WINAPI Win32Bridge::Bridge::BridgeAddRefActCtx(HANDLE context)
{
    if (const auto record = Lookup(context)) record->references.fetch_add(1);
}

void WINAPI Win32Bridge::Bridge::BridgeReleaseActCtx(HANDLE context)
{
    std::lock_guard<std::mutex> guard(g_contextLock);
    const auto found = g_contexts.find(reinterpret_cast<ULONG_PTR>(context));
    if (found != g_contexts.end() && found->second->references.fetch_sub(1) == 1)
        g_contexts.erase(found);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeActivateActCtx(HANDLE context, ULONG_PTR* cookie)
{
    if (!cookie || !Lookup(context))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const ULONG_PTR value = g_nextCookie.fetch_add(1);
    BridgeAddRefActCtx(context);
    g_activeContexts.push_back({ context, value });
    *cookie = value;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeactivateActCtx(DWORD flags, ULONG_PTR cookie)
{
    if ((flags & ~kDeactivateActCtxForceEarly) != 0 || g_activeContexts.empty())
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto found = std::find_if(g_activeContexts.rbegin(), g_activeContexts.rend(),
        [cookie](const ActiveRecord& item) { return item.cookie == cookie; });
    if (found == g_activeContexts.rend() ||
        (flags == 0 && found != g_activeContexts.rbegin()))
    {
        BridgeSetLastError(ERROR_SXS_EARLY_DEACTIVATION);
        return FALSE;
    }
    const size_t index = static_cast<size_t>(std::distance(found, g_activeContexts.rend()) - 1);
    const size_t first = (flags & kDeactivateActCtxForceEarly) ? index : g_activeContexts.size() - 1;
    for (size_t i = g_activeContexts.size(); i-- > first;)
        BridgeReleaseActCtx(g_activeContexts[i].context);
    g_activeContexts.erase(g_activeContexts.begin() + first, g_activeContexts.end());
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetCurrentActCtx(HANDLE* context)
{
    if (!context)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *context = CurrentHandle();
    if (*context) BridgeAddRefActCtx(*context);
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeQueryActCtxW(
    DWORD flags, HANDLE context, PVOID, ULONG informationClass,
    PVOID buffer, SIZE_T bufferSize, SIZE_T* requiredSize)
{
    const DWORD supportedFlags = kQueryActCtxUseActive | kQueryActCtxNoAddRef;
    if ((flags & ~supportedFlags) != 0)
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    HANDLE selected = (flags & kQueryActCtxUseActive) ? CurrentHandle() : context;
    const auto record = Lookup(selected);
    if (!record)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (informationClass)
    {
    case ActivationContextBasicInformation:
    {
        GuestActivationContextBasicInformation info{};
        info.hActCtx = selected;
        const BOOL returned = ReturnFixed(info, buffer, bufferSize, requiredSize);
        if (returned && (flags & kQueryActCtxNoAddRef) == 0) BridgeAddRefActCtx(selected);
        return returned;
    }
    case RunlevelInformationInActivationContext:
    {
        ACTIVATION_CONTEXT_RUN_LEVEL_INFORMATION info{};
        info.RunLevel = record->manifest.runLevel;
        info.UiAccess = record->manifest.uiAccess ? 1 : 0;
        return ReturnFixed(info, buffer, bufferSize, requiredSize);
    }
    case ActivationContextDetailedInformation:
    {
        const size_t sourceBytes = (record->manifest.source.size() + 1) * sizeof(wchar_t);
        const size_t required = sizeof(ACTIVATION_CONTEXT_DETAILED_INFORMATION) + sourceBytes;
        if (requiredSize) *requiredSize = required;
        if (!buffer || bufferSize < required)
        {
            BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        auto* info = static_cast<ACTIVATION_CONTEXT_DETAILED_INFORMATION*>(buffer);
        *info = ACTIVATION_CONTEXT_DETAILED_INFORMATION{};
        info->ulFormatVersion = 1;
        info->ulAssemblyCount = static_cast<ULONG>(1 + record->manifest.assemblies.size());
        info->ulRootManifestPathType = ACTIVATION_CONTEXT_PATH_TYPE_WIN32_FILE;
        info->ulRootManifestPathChars = static_cast<DWORD>(record->manifest.source.size());
        auto* text = reinterpret_cast<wchar_t*>(info + 1);
        memcpy(text, record->manifest.source.c_str(), sourceBytes);
        info->lpRootManifestPath = text;
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    case CompatibilityInformationInActivationContext:
    {
        const size_t count = record->manifest.supportedOperatingSystems.size();
        const size_t required = sizeof(ACTIVATION_CONTEXT_COMPATIBILITY_INFORMATION) + count * sizeof(COMPATIBILITY_CONTEXT_ELEMENT);
        if (requiredSize) *requiredSize = required;
        if (!buffer || bufferSize < required)
        {
            BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        auto* info = static_cast<ACTIVATION_CONTEXT_COMPATIBILITY_INFORMATION*>(buffer);
        info->ElementCount = static_cast<DWORD>(count);
        for (size_t i = 0; i < count; ++i)
        {
            info->Elements[i].Id = record->manifest.supportedOperatingSystems[i];
            info->Elements[i].Type = ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS;
            info->Elements[i].MaxVersionTested = 0;
        }
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    default:
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgeQueryActCtxSettingsW(
    DWORD flags,
    HANDLE context,
    LPCWSTR,
    LPCWSTR setting,
    LPWSTR buffer,
    SIZE_T bufferCount,
    SIZE_T* written)
{
    if (flags != 0 || !setting)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const auto record = Lookup(context ? context : CurrentHandle());
    if (!record)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::wstring value;
    if (_wcsicmp(setting, L"dpiAware") == 0 || _wcsicmp(setting, L"dpiAwareness") == 0)
        value = record->manifest.dpiAwareness;
    else if (_wcsicmp(setting, L"longPathAware") == 0)
        value = record->manifest.longPathAware ? L"true" : L"false";
    else
    {
        BridgeSetLastError(ERROR_SXS_KEY_NOT_FOUND);
        return FALSE;
    }
    if (value.empty())
    {
        BridgeSetLastError(ERROR_SXS_KEY_NOT_FOUND);
        return FALSE;
    }
    const size_t required = value.size() + 1;
    if (written) *written = required;
    if (!buffer || bufferCount < required)
    {
        BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buffer, value.c_str(), required * sizeof(wchar_t));
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}
