// Included inside Win32Bridge::Bridge. Native locale setters run only on a
// short-lived helper thread; neither the host global nor its caller PTD changes.
namespace {
void __cdecl IgnoreLocaleHelperValidation(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){}
std::shared_ptr<CrtNativeLocale> BuildCrtLocaleSnapshot(const CrtLocaleName& base,
    int category,const char* narrow,const wchar_t* wide,int codePage,int& failure){
    const int savedError=errno;unsigned long savedDos=0;::_get_doserrno(&savedDos);
    std::shared_ptr<CrtNativeLocale> result;
    failure=0;
    try{
        // The caller copies guest input before entering here. No guest pointers
        // or guest callbacks are dereferenced on this private native thread.
        std::thread worker([&]{
            const auto previous=::_set_thread_local_invalid_parameter_handler(&IgnoreLocaleHelperValidation);
            try{
                if(::_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)==-1||
                   !::_wsetlocale(LC_ALL,base->c_str())){failure=errno?errno:EINVAL;}
                else if((narrow&&!::setlocale(category,narrow))||(wide&&!::_wsetlocale(category,wide))){failure=errno?errno:EINVAL;}
                else{
                    auto value=std::make_shared<CrtNativeLocale>();
                    value->effectiveCodePage=codePage==_MB_CP_LOCALE?static_cast<int>(::___lc_codepage_func()):codePage;
                    if(::_setmbcp(value->effectiveCodePage)!=0){failure=errno?errno:EINVAL;}
                    else{
                        value->reportedCodePage=::_getmbcp();
                        for(int index=LC_ALL;index<=LC_TIME;++index){
                            const auto w=::_wsetlocale(index,nullptr);if(!w)throw std::bad_alloc();value->wideNames[index]=w;
                            const auto n=::setlocale(index,nullptr);if(!n)throw std::bad_alloc();value->narrowNames[index]=n;
                        }
                        value->name=std::make_shared<const std::wstring>(value->wideNames[LC_ALL]);
                        value->handle=::_get_current_locale();if(!value->handle)throw std::bad_alloc();
                        value->convention=::localeconv();if(!value->convention)throw std::bad_alloc();
                        result=std::move(value);
                    }
                }
            }catch(const std::bad_alloc&){failure=ENOMEM;}catch(...){failure=EINVAL;}
            ::_set_thread_local_invalid_parameter_handler(previous);
        });worker.join();
    }catch(const std::bad_alloc&){failure=ENOMEM;}catch(const std::system_error&){failure=EAGAIN;}
    ::_set_doserrno(savedDos);errno=savedError;return result;
}
bool LoadCrtLocaleSnapshot(CrtThreadExceptionState* thread,const CrtLocaleName& name){
    const auto page=thread->process->locales.MultibyteSnapshot(thread->locale);
    if(thread->locale.native&&thread->locale.native->name==name&&thread->locale.native->multibyte==page)return true;
    int failure=0;auto value=BuildCrtLocaleSnapshot(name,LC_ALL,nullptr,nullptr,*page,failure);
    if(!value){errno=failure?failure:ENOMEM;return false;}
    // Preserve policy identity for cache/transaction comparisons.
    value->name=name;value->multibyte=page;thread->locale.native=std::move(value);
    thread->process->locales.Observe(thread->locale,name,page);return true;
}
class CrtLocaleBoundary;
thread_local CrtLocaleBoundary* g_activeCrtLocaleBoundary=nullptr;
class CrtLocaleBoundary final {
public:
    CrtLocaleBoundary(CrtThreadExceptionState* selected,CrtThreadExceptionState*):
        m_selected(selected),m_outer(g_activeCrtLocaleBoundary){}
    bool Matches(CrtThreadExceptionState* thread) const {return m_selected==thread&&thread&&thread->process;}
    bool Synchronize(){
        if(!m_selected||!m_selected->process)return true;
        return SelectSnapshot(m_selected->process->locales.Snapshot(m_selected->locale));
    }
    bool SelectSnapshot(const CrtLocaleName& name){
        if(!LoadCrtLocaleSnapshot(m_selected,name))return false;
        const auto& value=m_selected->locale.native;
        // A native _l operation can invoke a guest validation callback which
        // changes locale or reenters another guest. Retain its old handle until
        // the protected callback exits, including longjmp/SEH cleanup paths.
        if(m_leases.empty()||m_leases.back()!=value)m_leases.push_back(value);
        return true;
    }
    // Merely entering a callback must not synchronize lazy CRT state before
    // _configthreadlocale can capture a new thread's initial private data.
    bool Enter(){g_activeCrtLocaleBoundary=this;return true;}
    bool Restore() noexcept {
        // clear() alone would leak the vector allocation when native SEH skips
        // the /EHsc wrapper destructor. Free both elements and capacity here.
        std::vector<std::shared_ptr<CrtNativeLocale>> empty;empty.swap(m_leases);
        g_activeCrtLocaleBoundary=m_outer;return true;
    }
private:
    CrtThreadExceptionState* m_selected;
    CrtLocaleBoundary* m_outer;
    std::vector<std::shared_ptr<CrtNativeLocale>> m_leases;
};
void InvokeCxxBody(CrtExecutionCallback callback,void* context,CrtLocaleBoundary* locale){
    try{if(locale&&!locale->Enter())DispatchGuestTerminate();callback(context);}
    catch(...){DispatchGuestTerminate();}
}
bool ValidCrtLocaleCategory(int category){
    if(category>=LC_ALL&&category<=LC_TIME)return true;
    ReportCrtInvalidParameter(L"valid locale category",L"setlocale",nullptr,0,0);errno=EINVAL;return false;
}
}
static bool SelectCrtLocaleSnapshot(CrtThreadExceptionState* thread,const CrtLocaleName& name){
    const int previous=errno;unsigned long dos=0;::_get_doserrno(&dos);
    bool result=false;try{
        result=g_activeCrtLocaleBoundary&&g_activeCrtLocaleBoundary->Matches(thread)?
            g_activeCrtLocaleBoundary->SelectSnapshot(name):LoadCrtLocaleSnapshot(thread,name);
    }catch(const std::bad_alloc&){errno=ENOMEM;}
    const int failure=errno?errno:ENOMEM;::_set_doserrno(dos);errno=result?previous:failure;return result;
}
bool SynchronizeCrtLocale(){
    const auto thread=g_guestThreadExceptions;if(!thread||!thread->process)return true;
    return SelectCrtLocaleSnapshot(thread,thread->process->locales.Snapshot(thread->locale));
}
_locale_t CurrentCrtLocaleHandle(){
    const auto thread=g_guestThreadExceptions;
    if(!thread||!thread->process)return nullptr; // Native semantics outside a guest.
    if(!SynchronizeCrtLocale())TerminateCrtExecution();
    return thread->locale.native->handle;
}
unsigned CurrentCrtConsoleSourcePage()
{
    const auto handle = CurrentCrtLocaleHandle();
    if (!handle || g_guestThreadExceptions->locale.native->wideNames[LC_CTYPE] == L"C") return 0;
    return CrtLocaleCodePage(handle);
}
int __cdecl ConfigureCrtLocale(int mode){
    const auto thread=g_guestThreadExceptions;if(!thread||!thread->process){errno=EINVAL;return -1;}
    if(mode!=-1&&mode!=0&&mode!=_ENABLE_PER_THREAD_LOCALE&&mode!=_DISABLE_PER_THREAD_LOCALE){
        ReportCrtInvalidParameter(L"valid public locale mode",L"_configthreadlocale",nullptr,0,0);errno=EINVAL;return -1;
    }
    auto& policy=thread->process->locales;const int previous=policy.Mode(thread->locale);
    if(!mode)return previous;
    // Selection is lazy. Configuration never changes a native host locale.
    return policy.Configure(thread->locale,mode);
}
namespace {
void SetCrtLocaleRequest(const std::string& name,const char*& narrow,const wchar_t*&){narrow=name.c_str();}
void SetCrtLocaleRequest(const std::wstring& name,const char*&,const wchar_t*& wide){wide=name.c_str();}
template<class Character> std::basic_string<Character> CopyCrtLocaleResult(const CrtNativeLocale&,int);
template<> std::string CopyCrtLocaleResult<char>(const CrtNativeLocale& value,int category){return value.narrowNames[category];}
template<> std::wstring CopyCrtLocaleResult<wchar_t>(const CrtNativeLocale& value,int category){return value.wideNames[category];}
char* StoreCrtLocaleResult(CrtLocaleThreadState& thread,std::string value){thread.result=std::move(value);return const_cast<char*>(thread.result.c_str());}
wchar_t* StoreCrtLocaleResult(CrtLocaleThreadState& thread,std::wstring value){thread.wideResult=std::move(value);return const_cast<wchar_t*>(thread.wideResult.c_str());}
}
template<class Character> Character* SetCrtLocaleName(int category,const Character* name){
    const auto thread=g_guestThreadExceptions;if(!thread||!thread->process){errno=EINVAL;return nullptr;}
    if(!ValidCrtLocaleCategory(category))return nullptr;
    const int saved=errno;unsigned long dos=0;::_get_doserrno(&dos);
    try{
        // Copy while still inside the guest's protected execution thread.
        std::basic_string<Character> requested;if(name)requested=name;
        auto& policy=thread->process->locales;
        for(;;){
            const auto expected=policy.Snapshot(thread->locale);
            if(!SelectCrtLocaleSnapshot(thread,expected))return nullptr;
            auto value=thread->locale.native;
            if(name){
                const char* narrow=nullptr;const wchar_t* wide=nullptr;
                SetCrtLocaleRequest(requested,narrow,wide);
                const auto page=policy.MultibyteSnapshot(thread->locale);
                int failure=0;value=BuildCrtLocaleSnapshot(expected,category,narrow,wide,*page,failure);
                if(!value){errno=failure?failure:EINVAL;return nullptr;}
                value->multibyte=page;
            }
            // Allocate return storage before publishing: allocation failure must
            // not report failure after silently committing a new guest locale.
            auto result=CopyCrtLocaleResult<Character>(*value,category);
            if(name&&!policy.TryCommit(thread->locale,expected,value->name))continue;
            if(name)thread->locale.native=value;
            Character* returned=StoreCrtLocaleResult(thread->locale,std::move(result));
            ::_set_doserrno(dos);errno=saved;return returned;
        }
    }catch(const std::bad_alloc&){::_set_doserrno(dos);errno=ENOMEM;return nullptr;}
}
char* __cdecl SetCrtLocale(int category,const char* name){return SetCrtLocaleName(category,name);}
wchar_t* __cdecl SetCrtWideLocale(int category,const wchar_t* name){return SetCrtLocaleName(category,name);}
lconv* __cdecl GetCrtLocaleConvention(){
    const auto thread=g_guestThreadExceptions;if(!thread||!thread->process){errno=EINVAL;return nullptr;}
    if(!SynchronizeCrtLocale())return nullptr;
    thread->locale.conventionLease=thread->locale.native;return thread->locale.native->convention;
}
_locale_t __cdecl GetCrtCurrentLocale(){
    const auto thread=g_guestThreadExceptions;
    if(!thread||!thread->process)return ::_get_current_locale();
    if(!SynchronizeCrtLocale())return nullptr;
    // Clone on a private helper so the caller owns an ordinary _locale_t
    // compatible with native _free_locale and its independent MBCS state.
    // Never copy private refcount fields or call setters on the caller PTD.
    int failure=0;auto value=BuildCrtLocaleSnapshot(thread->locale.native->name,LC_ALL,nullptr,nullptr,*thread->locale.native->multibyte,failure);
    if(!value){errno=failure?failure:ENOMEM;return nullptr;}
    const auto handle=value->handle;value->handle=nullptr;return handle;
}
int __cdecl GetCrtMultibyteCodePage(){
    const auto thread=g_guestThreadExceptions;
    if(!thread||!thread->process)return ::_getmbcp();
    if(!SynchronizeCrtLocale())return -1;
    return thread->locale.native->reportedCodePage;
}
int __cdecl SetCrtMultibyteCodePage(int codePage){
    const auto thread=g_guestThreadExceptions;if(!thread||!thread->process){errno=EINVAL;return -1;}
    if(codePage==_MB_CP_ANSI)codePage=static_cast<int>(GuestAnsiCodePage);
    else if(codePage==_MB_CP_OEM)codePage=static_cast<int>(GuestOemCodePage);
    if(codePage<0&&codePage!=_MB_CP_LOCALE){errno=EINVAL;return -1;}
    const int saved=errno;
    try{
        auto& policy=thread->process->locales;
        for(;;){
            const auto name=policy.Snapshot(thread->locale);
            const auto expected=policy.MultibyteSnapshot(thread->locale);
            int failure=0;auto value=BuildCrtLocaleSnapshot(name,LC_ALL,nullptr,nullptr,codePage,failure);
            if(!value){errno=failure?failure:EINVAL;return -1;}
            auto page=std::make_shared<const int>(value->effectiveCodePage);
            if(!policy.TryCommitMultibyte(thread->locale,expected,page))continue;
            value->name=name;value->multibyte=std::move(page);thread->locale.native=std::move(value);
            errno=saved;return 0;
        }
    }catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
}
