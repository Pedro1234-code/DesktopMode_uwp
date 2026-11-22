#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include "Bridge/CRT/CrtLocaleData.h"
#include "Bridge/CompatibilityCatalog.h"
#include "Bridge/CRT/CrtFormatting.h"
#include "Bridge/CRT/CrtScanning.h"
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <type_traits>
#include "Bridge/CRT/CrtLocaleExports.inl"
namespace {
struct HostLocaleIdentityModel {
    void* localeInfo;
    void* multibyteInfo;
    const wchar_t* name;
    lconv* convention;
    std::wstring nameValue;
    HostLocaleIdentityModel(){
        name=::_wsetlocale(LC_ALL,nullptr);nameValue=name;convention=::localeconv();
        const auto value=::_get_current_locale();
        localeInfo=value?value->locinfo:nullptr;multibyteInfo=value?value->mbcinfo:nullptr;
        if(value)::_free_locale(value);
    }
    bool Unchanged() const {
        const auto value=::_get_current_locale();
        const bool valid=value&&value->locinfo==localeInfo&&value->mbcinfo==multibyteInfo;
        if(value)::_free_locale(value);
        return valid&&nameValue==name&&::localeconv()==convention;
    }
};
struct HostLocaleRestoreModel {
    int mode=::_configthreadlocale(0);
    std::wstring name=::_wsetlocale(LC_ALL,nullptr);
    ~HostLocaleRestoreModel(){::_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);::_wsetlocale(LC_ALL,name.c_str());::_configthreadlocale(mode);}
};
struct LocaleReentryModel {
    std::weak_ptr<Win32Bridge::Bridge::CrtNativeLocale> original;
    bool valid=true;
    bool terminateOnValidation=false;
};
LocaleReentryModel* g_localeReentry=nullptr;
void __cdecl ChangeLocaleValidationModel(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){
    auto& value=*g_localeReentry;
    value.valid=Win32Bridge::Bridge::SetCrtLocale(LC_NUMERIC,"fr-FR")&&value.valid;
    value.valid=value.valid&&!value.original.expired();
    if(value.terminateOnValidation)Win32Bridge::Bridge::TerminateCrtExecution();
}
void __cdecl ReenterLocaleValidationModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& value=*static_cast<LocaleReentryModel*>(parameter);g_localeReentry=&value;
    CurrentCrtLocaleHandle();value.original=CurrentCrtThreadExceptionState()->locale.native;
    const auto previous=SetCrtThreadInvalidParameterHandler(&ChangeLocaleValidationModel);
    const auto result=CrtLocaleData__strlwr_s(nullptr,0);
    SetCrtThreadInvalidParameterHandler(previous);g_localeReentry=nullptr;
    value.valid=value.valid&&result==EINVAL&&!value.original.expired();
}
struct HostLocaleIsolationContext {HostLocaleIdentityModel* identity;bool valid=true;};
void __cdecl CheckHostLocaleIsolationModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& context=*static_cast<HostLocaleIsolationContext*>(parameter);auto& value=context.valid;
    value=value&&CrtLocaleData_strtod("1,5",nullptr)==1.0;
    value=value&&CrtLocaleData_towlower(0xD6)==0xD6&&::towlower(0xD6)==0xF6;
    value=SetCrtLocale(LC_CTYPE,"tr-TR")&&value;
    value=value&&CrtLocaleData_towlower(0xD6)==0xF6;
    value=value&&context.identity->Unchanged();
    value=SetCrtLocale(LC_ALL,"C")&&value;
}
struct LocaleModelContext {
    Win32Bridge::Bridge::CrtThreadExceptionState* worker;
    Win32Bridge::Bridge::CrtThreadExceptionState* first=nullptr;
    Win32Bridge::Bridge::CrtThreadExceptionState* other=nullptr;
    bool valid=true;
    char* returnedName=nullptr;
    lconv* convention=nullptr;
    _locale_t snapshot=nullptr;
};
int g_localeInvalid=0;
void __cdecl LocaleInvalidModelHandler(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){++g_localeInvalid;}
int FormatLocaleModel(char* output,size_t capacity,void* locale,const char* format,...){
    va_list args;va_start(args,format);
    const int result=Win32Bridge::Bridge::FormatCrtBuffer(0,output,capacity,capacity,format,locale,args,Win32Bridge::Bridge::CrtFormatFlavor::Standard);
    va_end(args);return result;
}
int ScanLocaleModel(const char* text,void* locale,const char* format,...){
    va_list args;va_start(args,format);
    const int result=Win32Bridge::Bridge::ScanCrtBuffer(0,text,std::strlen(text),format,locale,args);
    va_end(args);return result;
}
template<class Character> class LocaleScanMemoryModel final : public Win32Bridge::Bridge::CrtScanInput {
    std::basic_string<Character> text;
public:
    size_t cursor=0;
    explicit LocaleScanMemoryModel(std::basic_string<Character> value):text(std::move(value)){}
    int Get() override {return cursor<text.size()?static_cast<typename std::make_unsigned<Character>::type>(text[cursor++]):-1;}
    void Unget(int character) override {if(character>=0&&cursor)--cursor;}
};
template<class Character> int ScanStreamLocaleModel(Win32Bridge::Bridge::CrtScanInput& input,void* locale,const Character* format,...){
    va_list args;va_start(args,format);
    const int result=Win32Bridge::Bridge::ScanCrtStream(0,input,format,locale,args);
    va_end(args);return result;
}
void __cdecl SetSharedFrenchModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.returnedName=SetCrtLocale(LC_NUMERIC,"fr-FR");result.valid=result.returnedName!=nullptr&&ConfigureCrtLocale(0)==_DISABLE_PER_THREAD_LOCALE;
    result.convention=GetCrtLocaleConvention();result.valid=result.valid&&result.convention&&std::strcmp(result.convention->decimal_point,",")==0;
    result.snapshot=GetCrtCurrentLocale();result.valid=result.valid&&result.snapshot;
}
void __cdecl ObserveSharedModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
    const auto value=SetCrtWideLocale(LC_NUMERIC,nullptr);result.valid=result.valid&&value&&std::wcscmp(value,L"C")!=0;
}
void __cdecl PrivateThenSharedModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&ConfigureCrtLocale(_ENABLE_PER_THREAD_LOCALE)==_DISABLE_PER_THREAD_LOCALE;
    result.valid=SetCrtLocale(LC_NUMERIC,"C")&&result.valid;
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.0;
    char text[32]{};double scanned=0;
    result.valid=result.valid&&FormatLocaleModel(text,sizeof(text),result.snapshot,"%.1f",1.5)==3&&std::strcmp(text,"1,5")==0;
    result.valid=result.valid&&ScanLocaleModel("1,5",result.snapshot,"%lf",&scanned)==1&&scanned==1.5;
    result.valid=result.valid&&CrtLocaleData_strtod_l("1,5",nullptr,nullptr)==1.0;
    result.valid=result.valid&&CrtLocaleData_strtod_l("1,5",nullptr,result.snapshot)==1.5;
    LocaleScanMemoryModel<char> narrowStream("1,5 X");int consumed=0;
    result.valid=result.valid&&ScanStreamLocaleModel(narrowStream,result.snapshot,"%lf%n",&scanned,&consumed)==1&&scanned==1.5&&consumed==3&&narrowStream.cursor==3;
    LocaleScanMemoryModel<wchar_t> wideStream(L"1,5 X");
    result.valid=result.valid&&ScanStreamLocaleModel(wideStream,result.snapshot,L"%lf%n",&scanned,&consumed)==1&&scanned==1.5&&consumed==3&&wideStream.cursor==3;
    LocaleScanMemoryModel<char> defaultStream("1,5");
    result.valid=result.valid&&ScanStreamLocaleModel(defaultStream,nullptr,"%lf",&scanned)==1&&scanned==1.0&&defaultStream.cursor==1;
    result.valid=result.valid&&ConfigureCrtLocale(_DISABLE_PER_THREAD_LOCALE)==_ENABLE_PER_THREAD_LOCALE;
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
    LocaleScanMemoryModel<char> sharedStream("1,5");
    result.valid=result.valid&&ScanStreamLocaleModel(sharedStream,nullptr,"%lf",&scanned)==1&&scanned==1.5;
}
void __cdecl OtherProcessLocaleModel(void* parameter){
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&Win32Bridge::Bridge::CrtLocaleData_strtod("1,5",nullptr)==1.0;
}
void __cdecl ForcePrivateStartupLocaleModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&ConfigureCrtLocale(-1)==_DISABLE_PER_THREAD_LOCALE;
    result.valid=SetCrtLocale(LC_NUMERIC,"fr-FR")&&result.valid;
    result.valid=result.valid&&ConfigureCrtLocale(0)==_DISABLE_PER_THREAD_LOCALE&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
}
void __cdecl LocaleFailureModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);SetCrtThreadInvalidParameterHandler(&LocaleInvalidModelHandler);
    g_localeInvalid=0;const auto invalid=SetCrtLocale(999,"C");
    result.valid=result.valid&&!invalid&&errno==EINVAL&&g_localeInvalid==1;
    const auto badMode=ConfigureCrtLocale(7);result.valid=result.valid&&badMode==-1&&errno==EINVAL&&g_localeInvalid==2;
    const auto unknown=SetCrtLocale(LC_NUMERIC,"no-such-locale-987654");
    result.valid=result.valid&&!unknown&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
}
void __cdecl ResetSharedLocaleModel(void* parameter){
    auto& result=*static_cast<LocaleModelContext*>(parameter);result.valid=Win32Bridge::Bridge::SetCrtLocale(LC_ALL,"C")&&result.valid;
}
void __cdecl ConcurrentSharedLocaleModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&CrtLocaleData_strtod("1.5",nullptr)==1.5;
    bool workerValid=false;
    std::thread worker([&]{
        const auto previous=SetCrtThreadExceptionState(result.worker);
        workerValid=InvokeCrtExecution(&SetSharedFrenchModel,&result);
        SetCrtThreadExceptionState(previous);
    });worker.join();
    result.valid=result.valid&&workerValid&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
}
struct LocaleCategoryBarrier {std::mutex lock;std::condition_variable changed;unsigned ready=0;};
struct LocaleCategoryContext {LocaleCategoryBarrier* barrier;int category;const char* name;bool valid=false;};
void __cdecl SetConcurrentCategoryModel(void* parameter){
    auto& context=*static_cast<LocaleCategoryContext*>(parameter);
    {
        std::unique_lock<std::mutex> guard(context.barrier->lock);++context.barrier->ready;context.barrier->changed.notify_all();
        if(!context.barrier->changed.wait_for(guard,std::chrono::seconds(2),[&]{return context.barrier->ready==2;}))return;
    }
    context.valid=Win32Bridge::Bridge::SetCrtLocale(context.category,context.name)!=nullptr;
}
void __cdecl CheckConcurrentCategoriesModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& valid=*static_cast<bool*>(parameter);const auto name=SetCrtWideLocale(LC_CTYPE,nullptr);
    valid=valid&&name&&std::wcscmp(name,L"C")!=0&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
}
bool ValidateLocaleCategoryTransactions(Win32Bridge::Bridge::CrtExceptionProcessState& process){
    using namespace Win32Bridge::Bridge;
    CrtLocalePolicy policy;CrtLocaleThreadState leftModel,rightModel;
    const auto original=policy.Snapshot(leftModel);auto first=std::make_shared<const std::wstring>(L"fr-FR");
    if(!policy.TryCommit(leftModel,original,first))return false;
    if(policy.TryCommit(rightModel,original,std::make_shared<const std::wstring>(L"en-US")))return false;
    if(policy.Snapshot(rightModel)!=first)return false;
    bool valid=true;
    for(unsigned iteration=0;iteration<4;++iteration){
        LocaleModelContext reset{};valid=InvokeCrtExecution(&ResetSharedLocaleModel,&reset)&&reset.valid&&valid;
        LocaleCategoryBarrier barrier;LocaleCategoryContext left{&barrier,LC_CTYPE,"en-US"},right{&barrier,LC_NUMERIC,"fr-FR"};
        CrtThreadExceptionState leftState,rightState;leftState.process=rightState.process=&process;
        auto run=[](CrtThreadExceptionState* state,LocaleCategoryContext* context){
            const auto previous=SetCrtThreadExceptionState(state);
            const bool done=InvokeCrtExecution(&SetConcurrentCategoryModel,context);context->valid=context->valid&&done;
            SetCrtThreadExceptionState(previous);
        };
        std::thread firstThread(run,&leftState,&left),secondThread(run,&rightState,&right);firstThread.join();secondThread.join();
        valid=valid&&left.valid&&right.valid;valid=InvokeCrtExecution(&CheckConcurrentCategoriesModel,&valid)&&valid;
    }
    return valid;
}
void __cdecl ReenteredLocaleModel(void* parameter){
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=Win32Bridge::Bridge::SetCrtLocale(LC_NUMERIC,"C")&&result.valid;
}
void __cdecl InnerOtherLocaleModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.0;
    SetCrtThreadExceptionState(result.first);
    result.valid=InvokeCrtExecution(&ReenteredLocaleModel,&result)&&result.valid;
    SetCrtThreadExceptionState(result.other);
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.0;
}
void __cdecl CrossGuestLocaleModel(void* parameter){
    using namespace Win32Bridge::Bridge;
    auto& result=*static_cast<LocaleModelContext*>(parameter);
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.5;
    SetCrtThreadExceptionState(result.other);
    result.valid=InvokeCrtExecution(&InnerOtherLocaleModel,&result)&&result.valid;
    SetCrtThreadExceptionState(result.first);
    result.valid=result.valid&&CrtLocaleData_strtod("1,5",nullptr)==1.0;
}
void __cdecl FaultLocaleModel(void*){
    Win32Bridge::Bridge::SetCrtLocale(LC_NUMERIC,"fr-FR");
    ::RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);
}
bool ObserveLocaleFaultModel(){
    __try{Win32Bridge::Bridge::InvokeCrtExecution(&FaultLocaleModel,nullptr);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
    return false;
}
void __cdecl TerminateLocaleModel(void*){Win32Bridge::Bridge::SetCrtLocale(LC_NUMERIC,"C");Win32Bridge::Bridge::TerminateCrtExecution();}
bool ValidateCrtLocaleFixtures(){
    using namespace Win32Bridge::Bridge;
    HostLocaleRestoreModel restoreHost;
    ::_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    if(!::_wsetlocale(LC_ALL,L"fr-FR"))return false;
    CrtExceptionProcessState process,otherProcess;CrtThreadExceptionState first,second,other;
    first.process=second.process=&process;other.process=&otherProcess;
    const auto original=SetCrtThreadExceptionState(&first);
    const int hostMode=::_configthreadlocale(0);HostLocaleIdentityModel hostIdentity;
    LocaleModelContext result{&second};
    bool valid=true;
    HostLocaleIsolationContext isolation{&hostIdentity};
    valid=InvokeCrtExecution(&CheckHostLocaleIsolationModel,&isolation)&&isolation.valid&&valid&&hostIdentity.Unchanged();
    valid=InvokeCrtExecution(&SetSharedFrenchModel,&result)&&result.valid&&result.returnedName&&*result.returnedName&&valid;
    valid=valid&&result.convention&&std::strcmp(result.convention->decimal_point,",")==0;
    valid=valid&&::_configthreadlocale(0)==hostMode&&hostIdentity.Unchanged();
    SetCrtThreadExceptionState(&second);valid=InvokeCrtExecution(&PrivateThenSharedModel,&result)&&valid&&result.valid;
    SetCrtThreadExceptionState(&first);valid=InvokeCrtExecution(&ObserveSharedModel,&result)&&valid&&result.valid;
    SetCrtThreadExceptionState(&other);valid=InvokeCrtExecution(&OtherProcessLocaleModel,&result)&&valid&&result.valid;
    valid=InvokeCrtExecution(&ForcePrivateStartupLocaleModel,&result)&&valid&&result.valid;
    CrtThreadExceptionState otherNew;otherNew.process=&otherProcess;SetCrtThreadExceptionState(&otherNew);
    valid=InvokeCrtExecution(&OtherProcessLocaleModel,&result)&&valid&&result.valid;
    SetCrtThreadExceptionState(&first);valid=InvokeCrtExecution(&LocaleFailureModel,&result)&&valid&&result.valid;
    if(result.snapshot){::_free_locale(result.snapshot);result.snapshot=nullptr;}
    valid=InvokeCrtExecution(&ResetSharedLocaleModel,&result)&&valid&&result.valid;
    valid=InvokeCrtExecution(&ConcurrentSharedLocaleModel,&result)&&valid&&result.valid;
    valid=ValidateLocaleCategoryTransactions(process)&&valid;
    result.first=&first;result.other=&otherNew;
    valid=InvokeCrtExecution(&CrossGuestLocaleModel,&result)&&valid&&result.valid;
    const bool observedFault=ObserveLocaleFaultModel();
    valid=valid&&observedFault&&::_configthreadlocale(0)==hostMode&&hostIdentity.Unchanged();
    const bool completed=InvokeCrtExecution(&TerminateLocaleModel,nullptr);
    valid=valid&&!completed&&::_configthreadlocale(0)==hostMode&&hostIdentity.Unchanged();
    if(result.snapshot)::_free_locale(result.snapshot);
    ImportedSymbol symbol{};symbol.library=L"ucrtbase.dll";symbol.name=L"localeconv";
    valid=valid&&ResolveCrtLocaleImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&GetCrtLocaleConvention);
    symbol.library=L"kernel32.dll";valid=valid&&!ResolveCrtLocaleImport(symbol).targetAddress;
    symbol.library=L"ucrtbase.dll";symbol.name=L"_get_current_locale";
    valid=valid&&ResolveCrtLocaleImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&GetCrtCurrentLocale);
    CrtExceptionProcessState reentryProcess;CrtThreadExceptionState reentryThread;reentryThread.process=&reentryProcess;
    SetCrtThreadExceptionState(&reentryThread);LocaleReentryModel reentry;
    valid=InvokeCrtExecution(&ReenterLocaleValidationModel,&reentry)&&reentry.valid&&reentry.original.expired()&&valid;
    LocaleReentryModel terminatedReentry;terminatedReentry.terminateOnValidation=true;
    const bool nativeValidationReturned=InvokeCrtExecution(&ReenterLocaleValidationModel,&terminatedReentry);
    g_localeReentry=nullptr;SetCrtThreadInvalidParameterHandler(nullptr);
    valid=valid&&!nativeValidationReturned&&terminatedReentry.valid&&terminatedReentry.original.expired();
    valid=valid&&::_configthreadlocale(0)==hostMode&&hostIdentity.Unchanged();
    SetCrtThreadExceptionState(original);return valid;
}
}
