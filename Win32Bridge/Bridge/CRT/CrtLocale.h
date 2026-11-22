#pragma once
#include <locale.h>
#include "Bridge/GuestCodePages.h"
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
namespace Win32Bridge { namespace Bridge {
inline unsigned CrtLocaleCodePage(_locale_t locale)
{
    // The SDK exposes this prefix for inline locale-aware CRT access.
    return locale ? reinterpret_cast<const __crt_locale_data_public*>(locale->locinfo)->_locale_lc_codepage
                  : ::___lc_codepage_func();
}

using CrtLocaleName=std::shared_ptr<const std::wstring>;
using CrtMultibytePage=std::shared_ptr<const int>;
// A native locale reference pins localeconv data after the helper thread exits.
// Category names are copied; no pointers into native thread-local buffers escape.
struct CrtNativeLocale final {
    CrtLocaleName name;
    CrtMultibytePage multibyte;
    int effectiveCodePage=0;
    int reportedCodePage=0;
    _locale_t handle=nullptr;
    lconv* convention=nullptr;
    std::array<std::string,LC_TIME+1> narrowNames;
    std::array<std::wstring,LC_TIME+1> wideNames;
    ~CrtNativeLocale(){if(handle)::_free_locale(handle);}
    CrtNativeLocale()=default;
    CrtNativeLocale(const CrtNativeLocale&)=delete;
    CrtNativeLocale& operator=(const CrtNativeLocale&)=delete;
};
struct CrtLocaleThreadState {
    mbstate_t restartableCharacter{};
    mbstate_t restartableLength{};
    mbstate_t restartableString{};
    mbstate_t utf8ToUtf16{};
    mbstate_t utf8ToUtf32{};
    mbstate_t utf16ToUtf8{};
    mbstate_t utf32ToUtf8{};
    int mode=_DISABLE_PER_THREAD_LOCALE;
    CrtLocaleName privateName;
    CrtLocaleName observedName;
    CrtMultibytePage privateMultibyte;
    CrtMultibytePage observedMultibyte;
    std::string result;
    std::wstring wideResult;
    // Keep localeconv's referenced native locale data alive across boundaries.
    std::shared_ptr<CrtNativeLocale> conventionLease;
    std::shared_ptr<CrtNativeLocale> native;
};
class CrtLocalePolicy final {
public:
    CrtLocalePolicy():m_global(std::make_shared<const std::wstring>(L"C")),m_globalMultibyte(std::make_shared<const int>(GuestAnsiCodePage)),m_initialMultibyte(std::make_shared<const int>(0)){}
    void InitializeThread(CrtLocaleThreadState& thread,bool initialThread=false) const {
        std::lock_guard<std::mutex> guard(m_lock);
        if(!thread.observedName)thread.observedName=m_global;
        if(!thread.observedMultibyte)thread.observedMultibyte=initialThread?m_globalMultibyte:m_initialMultibyte;
    }
    CrtLocaleName Snapshot(const CrtLocaleThreadState& thread) const {
        std::lock_guard<std::mutex> guard(m_lock);
        if(thread.mode==_ENABLE_PER_THREAD_LOCALE&&thread.privateName)return thread.privateName;
        if(m_forcePrivate)return thread.privateName?thread.privateName:thread.observedName?thread.observedName:m_global;
        return m_global;
    }
    CrtMultibytePage MultibyteSnapshot(const CrtLocaleThreadState& thread) const {
        std::lock_guard<std::mutex> guard(m_lock);return MultibyteUnlocked(thread);
    }
    int Mode(const CrtLocaleThreadState& thread) const {std::lock_guard<std::mutex> guard(m_lock);return thread.mode;}
    int Configure(CrtLocaleThreadState& thread,int mode){
        std::lock_guard<std::mutex> guard(m_lock);const int previous=thread.mode;
        if(mode==-1)m_forcePrivate=true; // UCRT startup disables global synchronization.
        else if(mode==_ENABLE_PER_THREAD_LOCALE&&previous!=mode){
            thread.privateMultibyte=thread.privateMultibyte?thread.privateMultibyte:thread.observedMultibyte?thread.observedMultibyte:m_initialMultibyte;
            thread.privateName=thread.privateName?thread.privateName:thread.observedName?thread.observedName:m_global;thread.mode=mode;
        }else if(mode==_DISABLE_PER_THREAD_LOCALE){thread.mode=mode;thread.privateName.reset();thread.privateMultibyte.reset();}
        return previous;
    }
    bool TryCommit(CrtLocaleThreadState& thread,const CrtLocaleName& expected,CrtLocaleName name){
        std::lock_guard<std::mutex> guard(m_lock);
        CrtLocaleName current=m_global;
        if(thread.mode==_ENABLE_PER_THREAD_LOCALE&&thread.privateName)current=thread.privateName;
        else if(m_forcePrivate)current=thread.privateName?thread.privateName:thread.observedName?thread.observedName:m_global;
        if(current!=expected)return false;
        thread.observedName=name;
        if(thread.mode==_ENABLE_PER_THREAD_LOCALE||m_forcePrivate)thread.privateName=std::move(name);else m_global=std::move(name);
        return true;
    }
    bool TryCommitMultibyte(CrtLocaleThreadState& thread,const CrtMultibytePage& expected,CrtMultibytePage page){
        std::lock_guard<std::mutex> guard(m_lock);
        if(MultibyteUnlocked(thread)!=expected)return false;
        thread.observedMultibyte=page;
        if(thread.mode==_ENABLE_PER_THREAD_LOCALE||m_forcePrivate)thread.privateMultibyte=std::move(page);
        else m_globalMultibyte=std::move(page);
        return true;
    }
    void Observe(CrtLocaleThreadState& thread,const CrtLocaleName& name,const CrtMultibytePage& page) const {
        std::lock_guard<std::mutex> guard(m_lock);thread.observedName=name;thread.observedMultibyte=page;
    }
private:
    CrtMultibytePage MultibyteUnlocked(const CrtLocaleThreadState& thread) const {
        if(thread.mode==_ENABLE_PER_THREAD_LOCALE&&thread.privateMultibyte)return thread.privateMultibyte;
        if(m_forcePrivate)return thread.privateMultibyte?thread.privateMultibyte:thread.observedMultibyte?thread.observedMultibyte:m_globalMultibyte;
        return m_globalMultibyte;
    }
    mutable std::mutex m_lock;
    CrtLocaleName m_global;
    CrtMultibytePage m_globalMultibyte;
    CrtMultibytePage m_initialMultibyte;
    bool m_forcePrivate=false;
};
} }
