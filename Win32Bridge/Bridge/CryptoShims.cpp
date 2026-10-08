#include "pch.h"
#include "Bridge/CryptoShims.h"
#include "Bridge/Kernel32Shims.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    NTSTATUS WINAPI BridgeBCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* result, LPCWSTR algorithm,
        LPCWSTR provider, ULONG flags) { return ::BCryptOpenAlgorithmProvider(result, algorithm, provider, flags); }
    NTSTATUS WINAPI BridgeBCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE handle, ULONG flags)
        { return ::BCryptCloseAlgorithmProvider(handle, flags); }
    NTSTATUS WINAPI BridgeBCryptGetProperty(BCRYPT_HANDLE handle, LPCWSTR property, PUCHAR output,
        ULONG outputBytes, ULONG* resultBytes, ULONG flags)
        { return ::BCryptGetProperty(handle, property, output, outputBytes, resultBytes, flags); }
    NTSTATUS WINAPI BridgeBCryptCreateHash(BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE* result,
        PUCHAR object, ULONG objectBytes, PUCHAR secret, ULONG secretBytes, ULONG flags)
        { return ::BCryptCreateHash(algorithm, result, object, objectBytes, secret, secretBytes, flags); }
    NTSTATUS WINAPI BridgeBCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG bytes, ULONG flags)
        { return ::BCryptHashData(hash, input, bytes, flags); }
    NTSTATUS WINAPI BridgeBCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG bytes, ULONG flags)
        { return ::BCryptFinishHash(hash, output, bytes, flags); }
    NTSTATUS WINAPI BridgeBCryptDestroyHash(BCRYPT_HASH_HANDLE hash) { return ::BCryptDestroyHash(hash); }
    NTSTATUS WINAPI BridgeBCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, PUCHAR output, ULONG bytes, ULONG flags)
        { return ::BCryptGenRandom(algorithm, output, bytes, flags); }

    struct LegacyProvider {};
    struct LegacyHash
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ULONG digestBytes = 0;
        std::vector<BYTE> digest;
    };
    std::mutex g_cryptoLock;
    std::unordered_map<ULONG_PTR, std::unique_ptr<LegacyProvider>> g_providers;
    std::unordered_map<ULONG_PTR, std::unique_ptr<LegacyHash>> g_hashes;
    ULONG_PTR g_nextCryptoHandle = 0x76000000;

    LPCWSTR AlgorithmForId(ALG_ID id)
    {
        switch (id)
        {
        case CALG_MD5: return BCRYPT_MD5_ALGORITHM;
        case CALG_SHA1: return BCRYPT_SHA1_ALGORITHM;
        case CALG_SHA_256: return BCRYPT_SHA256_ALGORITHM;
        case CALG_SHA_384: return BCRYPT_SHA384_ALGORITHM;
        case CALG_SHA_512: return BCRYPT_SHA512_ALGORITHM;
        default: return nullptr;
        }
    }

    BOOL WINAPI BridgeCryptAcquireContextW(HCRYPTPROV* result, LPCWSTR, LPCWSTR, DWORD, DWORD)
    {
        if (!result) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        auto provider = std::make_unique<LegacyProvider>();
        std::lock_guard<std::mutex> guard(g_cryptoLock);
        const ULONG_PTR token = ++g_nextCryptoHandle;
        g_providers[token] = std::move(provider);
        *result = static_cast<HCRYPTPROV>(token);
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    BOOL WINAPI BridgeCryptReleaseContext(HCRYPTPROV provider, DWORD)
    {
        std::lock_guard<std::mutex> guard(g_cryptoLock);
        return g_providers.erase(static_cast<ULONG_PTR>(provider)) != 0;
    }

    BOOL WINAPI BridgeCryptCreateHash(HCRYPTPROV provider, ALG_ID id, HCRYPTKEY, DWORD, HCRYPTHASH* result)
    {
        if (!result) return FALSE;
        const LPCWSTR algorithmName = AlgorithmForId(id);
        if (!algorithmName) { BridgeSetLastError(NTE_BAD_ALGID); return FALSE; }
        auto record = std::make_unique<LegacyHash>();
        if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&record->algorithm, algorithmName, nullptr, 0))) return FALSE;
        ULONG returned = 0;
        if (!BCRYPT_SUCCESS(::BCryptGetProperty(record->algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&record->digestBytes), sizeof(record->digestBytes), &returned, 0)) ||
            !BCRYPT_SUCCESS(::BCryptCreateHash(record->algorithm, &record->hash, nullptr, 0, nullptr, 0, 0)))
        {
            if (record->algorithm) ::BCryptCloseAlgorithmProvider(record->algorithm, 0);
            return FALSE;
        }
        std::lock_guard<std::mutex> guard(g_cryptoLock);
        if (g_providers.find(static_cast<ULONG_PTR>(provider)) == g_providers.end()) return FALSE;
        const ULONG_PTR token = ++g_nextCryptoHandle;
        g_hashes[token] = std::move(record);
        *result = static_cast<HCRYPTHASH>(token);
        return TRUE;
    }

    BOOL WINAPI BridgeCryptHashData(HCRYPTHASH handle, const BYTE* data, DWORD bytes, DWORD)
    {
        std::lock_guard<std::mutex> guard(g_cryptoLock);
        const auto found = g_hashes.find(static_cast<ULONG_PTR>(handle));
        return found != g_hashes.end() && found->second->digest.empty() &&
            BCRYPT_SUCCESS(::BCryptHashData(found->second->hash, const_cast<PUCHAR>(data), bytes, 0));
    }

    BOOL FinishLegacyHash(LegacyHash* hash)
    {
        if (!hash) return FALSE;
        if (!hash->digest.empty()) return TRUE;
        hash->digest.resize(hash->digestBytes);
        return BCRYPT_SUCCESS(::BCryptFinishHash(hash->hash, hash->digest.data(), hash->digestBytes, 0));
    }

    BOOL WINAPI BridgeCryptGetHashParam(HCRYPTHASH handle, DWORD parameter, BYTE* data, DWORD* bytes, DWORD)
    {
        if (!bytes) return FALSE;
        std::lock_guard<std::mutex> guard(g_cryptoLock);
        const auto found = g_hashes.find(static_cast<ULONG_PTR>(handle));
        if (found == g_hashes.end()) return FALSE;
        if (parameter == HP_HASHSIZE)
        {
            const DWORD required = sizeof(DWORD);
            if (!data || *bytes < required) { *bytes = required; BridgeSetLastError(ERROR_MORE_DATA); return FALSE; }
            memcpy(data, &found->second->digestBytes, required); *bytes = required; return TRUE;
        }
        if (parameter != HP_HASHVAL || !FinishLegacyHash(found->second.get())) return FALSE;
        const DWORD required = found->second->digestBytes;
        if (!data || *bytes < required) { *bytes = required; BridgeSetLastError(ERROR_MORE_DATA); return FALSE; }
        memcpy(data, found->second->digest.data(), required); *bytes = required; return TRUE;
    }

    BOOL WINAPI BridgeCryptDestroyHash(HCRYPTHASH handle)
    {
        std::unique_ptr<LegacyHash> removed;
        {
            std::lock_guard<std::mutex> guard(g_cryptoLock);
            const auto found = g_hashes.find(static_cast<ULONG_PTR>(handle));
            if (found == g_hashes.end()) return FALSE;
            removed = std::move(found->second); g_hashes.erase(found);
        }
        if (removed->hash) ::BCryptDestroyHash(removed->hash);
        if (removed->algorithm) ::BCryptCloseAlgorithmProvider(removed->algorithm, 0);
        return TRUE;
    }

    BOOL WINAPI BridgeCryptGenRandom(HCRYPTPROV provider, DWORD bytes, BYTE* output)
    {
        { std::lock_guard<std::mutex> guard(g_cryptoLock); if (g_providers.find(static_cast<ULONG_PTR>(provider)) == g_providers.end()) return FALSE; }
        return BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, output, bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    ULONG_PTR g_nextImmContext = 0x77000000;
    std::mutex g_immLock;
    std::unordered_map<ULONG_PTR, HWND> g_immContexts;
    HANDLE WINAPI BridgeImmGetContext(HWND window)
    {
        if (!window) return nullptr;
        std::lock_guard<std::mutex> guard(g_immLock);
        const ULONG_PTR token = ++g_nextImmContext; g_immContexts[token] = window;
        return reinterpret_cast<HANDLE>(token);
    }
    BOOL WINAPI BridgeImmReleaseContext(HWND, HANDLE context)
    { std::lock_guard<std::mutex> guard(g_immLock); return g_immContexts.erase(reinterpret_cast<ULONG_PTR>(context)) != 0; }
    LONG WINAPI BridgeImmGetCompositionStringW(HANDLE, DWORD, LPVOID, DWORD) { return 0; }
    BOOL WINAPI BridgeImmBooleanStub(HANDLE, DWORD, DWORD, DWORD) { return TRUE; }
    BOOL WINAPI BridgeImmPointerStub(HANDLE, const void*) { return TRUE; }
    LRESULT WINAPI BridgeImmEscapeW(HKL, HANDLE, UINT, LPVOID) { return 0; }

    struct EmptyCertificateStore {};
    HANDLE WINAPI BridgeCertOpenStore(LPCSTR, DWORD, ULONG_PTR, DWORD, const void*)
        { return new (std::nothrow) EmptyCertificateStore(); }
    BOOL WINAPI BridgeCertCloseStore(HANDLE store, DWORD) { delete static_cast<EmptyCertificateStore*>(store); return TRUE; }
    const void* WINAPI BridgeCertFindCertificateInStore(HANDLE, DWORD, DWORD, DWORD, const void*, const void*)
        { BridgeSetLastError(static_cast<DWORD>(CRYPT_E_NOT_FOUND)); return nullptr; }
    BOOL WINAPI BridgeCryptFalseStub() { BridgeSetLastError(static_cast<DWORD>(CRYPT_E_NO_MATCH)); return FALSE; }
    BOOL WINAPI BridgeCryptMsgClose(HANDLE message) { return message != nullptr; }
    DWORD WINAPI BridgeCertGetNameStringW(const void*, DWORD, DWORD, void*, LPWSTR output, DWORD count)
        { if (output && count) output[0] = L'\0'; return count ? 1 : 1; }
    DWORD WINAPI BridgeCertNameToStrW(DWORD, const void*, DWORD, LPWSTR output, DWORD count)
        { if (output && count) output[0] = L'\0'; return 1; }
}

ImportResolution Win32Bridge::Bridge::ResolveCryptoImport(const ImportedSymbol& symbol)
{
    ImportResolution result = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal) return result;
    if (_wcsicmp(symbol.library.c_str(), L"bcrypt.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"bcryptgetproperty") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptGetProperty);
        else if (_wcsicmp(symbol.name.c_str(), L"bcryptopenalgorithmprovider") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptOpenAlgorithmProvider);
        else if (_wcsicmp(symbol.name.c_str(), L"bcryptfinishhash") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptFinishHash);
        else if (_wcsicmp(symbol.name.c_str(), L"bcryptclosealgorithmprovider") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptCloseAlgorithmProvider);
        else if (_wcsicmp(symbol.name.c_str(), L"bcryptcreatehash") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptCreateHash);
        else if (_wcsicmp(symbol.name.c_str(), L"bcrypthashdata") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptHashData);
        else if (_wcsicmp(symbol.name.c_str(), L"bcryptdestroyhash") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptDestroyHash);
        else if (_wcsicmp(symbol.name.c_str(), L"bcryptgenrandom") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBCryptGenRandom);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"advapi32.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"cryptacquirecontextw") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptAcquireContextW);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptreleasecontext") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptReleaseContext);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptcreatehash") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptCreateHash);
        else if (_wcsicmp(symbol.name.c_str(), L"crypthashdata") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptHashData);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptgethashparam") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptGetHashParam);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptdestroyhash") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptDestroyHash);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptgenrandom") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptGenRandom);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"imm32.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"immgetcontext") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmGetContext);
        else if (_wcsicmp(symbol.name.c_str(), L"immreleasecontext") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmReleaseContext);
        else if (_wcsicmp(symbol.name.c_str(), L"immgetcompositionstringw") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmGetCompositionStringW);
        else if (_wcsicmp(symbol.name.c_str(), L"immnotifyime") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmBooleanStub);
        else if (_wcsicmp(symbol.name.c_str(), L"immsetcandidatewindow") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmPointerStub);
        else if (_wcsicmp(symbol.name.c_str(), L"immsetcompositionstringw") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmBooleanStub);
        else if (_wcsicmp(symbol.name.c_str(), L"immescapew") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmEscapeW);
        else if (_wcsicmp(symbol.name.c_str(), L"immsetcompositionwindow") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmPointerStub);
        else if (_wcsicmp(symbol.name.c_str(), L"immsetcompositionfontw") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImmPointerStub);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"crypt32.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"certopenstore") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCertOpenStore);
        else if (_wcsicmp(symbol.name.c_str(), L"certclosestore") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCertCloseStore);
        else if (_wcsicmp(symbol.name.c_str(), L"certfindcertificateinstore") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCertFindCertificateInStore);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptmsgclose") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptMsgClose);
        else if (_wcsicmp(symbol.name.c_str(), L"certgetnamestringw") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCertGetNameStringW);
        else if (_wcsicmp(symbol.name.c_str(), L"certnametostrw") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCertNameToStrW);
        else if (_wcsicmp(symbol.name.c_str(), L"certgetcertificatecontextproperty") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptFalseStub);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptmsggetparam") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptFalseStub);
        else if (_wcsicmp(symbol.name.c_str(), L"cryptqueryobject") == 0) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCryptFalseStub);
    }
    if (result.targetAddress) { result.disposition = ImportDisposition::NeedsBridge; result.note = L"Isolated guest crypto/certificate/IME adapter."; }
    return result;
}
