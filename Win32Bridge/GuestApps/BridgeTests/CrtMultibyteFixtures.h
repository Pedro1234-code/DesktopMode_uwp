#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include "Bridge/CRT/CrtData.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
namespace {
using MbThreadState=Win32Bridge::Bridge::CrtThreadExceptionState;
using MbProcessState=Win32Bridge::Bridge::CrtExceptionProcessState;
struct MbFixtureContext {bool valid=true;MbProcessState* process=nullptr;MbThreadState* other=nullptr;};
bool RunMbFixture(MbThreadState& state,Win32Bridge::Bridge::CrtExecutionCallback callback,void* context){
    using namespace Win32Bridge::Bridge;
    const auto previous=SetCrtThreadExceptionState(&state);const bool done=InvokeCrtExecution(callback,context);
    SetCrtThreadExceptionState(previous);return done;
}
void __cdecl MbPrivateFreshThread(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbFixtureContext*>(parameter);
    test.valid=(ConfigureCrtLocale(_ENABLE_PER_THREAD_LOCALE)==_DISABLE_PER_THREAD_LOCALE)&&test.valid;
    // No MBCS observation preceded private mode: retain initial SBCS data.
    test.valid=(GetCrtMultibyteCodePage()==0&&!CrtMb_ismbblead(0x82)&&CrtMb_mbctoupper(0xFF)==0xFF)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(1251)==0&&CrtMb_mbctoupper(0xFF)==0xDF)&&test.valid;
    test.valid=(ConfigureCrtLocale(_DISABLE_PER_THREAD_LOCALE)==_ENABLE_PER_THREAD_LOCALE)&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==932)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(1251)==0)&&test.valid;
}
void __cdecl MbSharedPeer(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbFixtureContext*>(parameter);
    test.valid=(GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0xDF)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(1252)==0)&&test.valid;
}
void __cdecl MbOtherProcess(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbFixtureContext*>(parameter);
    test.valid=(GetCrtMultibyteCodePage()==0&&!CrtMb_ismbblead(0x82)&&CrtMb_mbctoupper(0xFF)==0x9F)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(936)==0)&&test.valid;
}
struct MbCategoryBarrier {std::mutex lock;std::condition_variable changed;unsigned ready=0;};
struct MbCategoryContext {MbCategoryBarrier* barrier;bool multibyte;bool valid=false;};
void __cdecl MbConcurrentCategory(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbCategoryContext*>(parameter);
    {
        std::unique_lock<std::mutex> guard(test.barrier->lock);++test.barrier->ready;test.barrier->changed.notify_all();
        if(!test.barrier->changed.wait_for(guard,std::chrono::seconds(5),[&]{return test.barrier->ready==2;}))return;
    }
    test.valid=test.multibyte?SetCrtMultibyteCodePage(932)==0:SetCrtLocale(LC_NUMERIC,"fr-FR")!=nullptr;
}
void __cdecl MbRootContract(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbFixtureContext*>(parameter);
    const unsigned char text[]={0x82,0xA0,'A',0};unsigned char copied[8]{};
    test.valid=(GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0x9F)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(932)==0&&GetCrtMultibyteCodePage()==932)&&test.valid;
    test.valid=(CrtMb_ismbblead(0x82)&&CrtMb_ismbbtrail(0xA0)&&CrtMb_mbslen(text)==2&&CrtMb_mbsinc(text)==text+2)&&test.valid;
    test.valid=(CrtMb_mbsnbcnt(text,1)==2&&CrtMb_mbsnccnt(text,2)==1)&&test.valid;
    test.valid=(CrtMb_mbsncpy_s(copied,sizeof(copied),text,1)==0&&!std::memcmp(copied,text,2)&&copied[2]==0)&&test.valid;
    auto owned=GetCrtCurrentLocale();test.valid=(owned!=nullptr)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(0)==0&&!CrtMb_ismbblead(0x82))&&test.valid;
    if(owned){test.valid=(CrtMb_ismbblead_l(0x82,owned)!=0)&&test.valid;::_free_locale(owned);}
    test.valid=(SetCrtLocale(LC_CTYPE,"Japanese_Japan.932")!=nullptr)&&test.valid;
    wchar_t wide=0;
    test.valid=(!CrtMb_ismbblead(0x82)&&CrtLocaleConvert_mbtowc(&wide,reinterpret_cast<const char*>(text),2)==2&&wide==0x3042)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(_MB_CP_LOCALE)==0&&GetCrtMultibyteCodePage()==932)&&test.valid;
    test.valid=(SetCrtLocale(LC_CTYPE,"C")!=nullptr)&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==932&&CrtLocaleConvert_mbtowc(&wide,reinterpret_cast<const char*>(text),2)==1&&wide==0x82)&&test.valid;
    errno=0;test.valid=(SetCrtMultibyteCodePage(65000)==-1&&errno==EINVAL&&GetCrtMultibyteCodePage()==932)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(65001)==0&&GetCrtMultibyteCodePage()==0&&!CrtMb_ismbblead(0xC3))&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(1251)==0&&GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0xDF)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(1252)==0&&GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0x9F)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(_MB_CP_ANSI)==0&&CrtMb_mbctoupper(0xFF)==0x9F)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(_MB_CP_OEM)==0&&GetCrtMultibyteCodePage()==0)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(932)==0)&&test.valid;
    bool peerDone=false;
    std::thread privateWorker([&]{MbThreadState peer;peer.process=test.process;peerDone=RunMbFixture(peer,&MbPrivateFreshThread,&test);});privateWorker.join();
    test.valid=peerDone&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0xDF)&&test.valid;
    test.valid=(ConfigureCrtLocale(_ENABLE_PER_THREAD_LOCALE)==_DISABLE_PER_THREAD_LOCALE)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(932)==0)&&test.valid;
    std::thread sharedWorker([&]{MbThreadState peer;peer.process=test.process;peerDone=RunMbFixture(peer,&MbSharedPeer,&test);});sharedWorker.join();
    test.valid=peerDone&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==932)&&test.valid;
    test.valid=(ConfigureCrtLocale(_DISABLE_PER_THREAD_LOCALE)==_ENABLE_PER_THREAD_LOCALE)&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0x9F)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(932)==0)&&test.valid;
    test.valid=RunMbFixture(*test.other,&MbOtherProcess,&test)&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==932)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(0)==0&&SetCrtLocale(LC_ALL,"C"))&&test.valid;
    MbCategoryBarrier barrier;MbCategoryContext numeric{&barrier,false},multibyte{&barrier,true};
    auto run=[&](MbCategoryContext& context){MbThreadState peer;peer.process=test.process;context.valid=RunMbFixture(peer,&MbConcurrentCategory,&context)&&context.valid;};
    std::thread left([&]{run(numeric);}),right([&]{run(multibyte);});left.join();right.join();
    test.valid=numeric.valid&&multibyte.valid&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==932&&CrtLocaleData_strtod("1,5",nullptr)==1.5)&&test.valid;
}
void __cdecl MbForcedFreshThread(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbFixtureContext*>(parameter);
    test.valid=(ConfigureCrtLocale(0)==_DISABLE_PER_THREAD_LOCALE&&GetCrtMultibyteCodePage()==0&&CrtMb_mbctoupper(0xFF)==0xFF)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(1251)==0)&&test.valid;
}
void __cdecl MbForcedRoot(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbFixtureContext*>(parameter);
    test.valid=(SetCrtMultibyteCodePage(932)==0&&ConfigureCrtLocale(-1)==_DISABLE_PER_THREAD_LOCALE)&&test.valid;
    test.valid=(SetCrtMultibyteCodePage(936)==0)&&test.valid;
    bool done=false;std::thread worker([&]{MbThreadState state;state.process=test.process;done=RunMbFixture(state,&MbForcedFreshThread,&test);});worker.join();
    test.valid=done&&test.valid;
    test.valid=(GetCrtMultibyteCodePage()==936&&ConfigureCrtLocale(0)==_DISABLE_PER_THREAD_LOCALE)&&test.valid;
}
struct MbReentryContext {std::weak_ptr<Win32Bridge::Bridge::CrtNativeLocale> original;bool valid=true;};
MbReentryContext* g_mbReentry=nullptr;
void __cdecl MbValidationReentry(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){
    auto& test=*g_mbReentry;test.valid=(Win32Bridge::Bridge::SetCrtMultibyteCodePage(1251)==0)&&test.valid;
    test.valid=!test.original.expired()&&test.valid;
}
void __cdecl MbReentryContract(void* parameter){
    using namespace Win32Bridge::Bridge;auto& test=*static_cast<MbReentryContext*>(parameter);g_mbReentry=&test;
    CurrentCrtLocaleHandle();test.original=CurrentCrtThreadExceptionState()->locale.native;
    const auto previous=SetCrtThreadInvalidParameterHandler(&MbValidationReentry);
    const unsigned char text[]={'x',0};const auto result=CrtMb_mbscpy_s(nullptr,0,text);
    SetCrtThreadInvalidParameterHandler(previous);g_mbReentry=nullptr;
    test.valid=(result==EINVAL&&!test.original.expired())&&test.valid;
}
void __cdecl MbRestartableOther(void* parameter)
{
    using namespace Win32Bridge::Bridge;
    auto& valid = *static_cast<bool*>(parameter);
    wchar_t wide = 0;
    valid = CrtRestartable_mbrtowc(&wide, "\x82", 1, nullptr) == 1 && wide == 0x82 && valid;
}

void __cdecl MbRestartableContract(void* parameter)
{
    using namespace Win32Bridge::Bridge;
    auto& test = *static_cast<MbFixtureContext*>(parameter);
    constexpr auto incomplete = static_cast<size_t>(-2);
    constexpr auto invalid = static_cast<size_t>(-1);
    if (!SetCrtLocale(LC_CTYPE, "Japanese_Japan.932") || SetCrtMultibyteCodePage(0) != 0)
    {
        test.valid = false;
        return;
    }
    wchar_t wide = 0;
    test.valid = CrtRestartable_mbrtowc(&wide, "\x82", 1, nullptr) == incomplete && test.valid;
    test.valid = RunMbFixture(*test.other, &MbRestartableOther, &test.valid) && test.valid;
    test.valid = CrtRestartable_mbrtowc(&wide, "\xa0", 1, nullptr) == 2 && wide == 0x3042 && test.valid;
    test.valid = CrtRestartable_mbrlen("\x82", 1, nullptr) == incomplete && test.valid;
    test.valid = CrtRestartable_mbrtowc(&wide, "A", 1, nullptr) == 1 && wide == L'A' && test.valid;
    test.valid = CrtRestartable_mbrlen("\xa0", 1, nullptr) == 2 && test.valid;

    if (!SetCrtLocale(LC_CTYPE, ".UTF-8"))
    {
        test.valid = false;
        return;
    }
    mbstate_t state{};
    test.valid = CrtRestartable_mbrtowc(&wide, "\xe2", 1, &state) == incomplete && !CrtRestartable_mbsinit(&state) && test.valid;
    test.valid = CrtRestartable_mbrtowc(&wide, "\x82\xac", 2, &state) == 2 && wide == 0x20ac && CrtRestartable_mbsinit(&state) && test.valid;
    test.valid = CrtRestartable_mbrtowc(&wide, "\xf0\x9f\x98\x80", 4, &state) == 4 && wide == 0xfffd && test.valid;
    test.valid = CrtRestartable_mbrtowc(&wide, "\xf4\x90\x80\x80", 4, &state) == invalid && errno == EILSEQ && test.valid;

    const char text[] = "\xf0\x9f\x98\x80" "A";
    const char* input = text;
    wchar_t output[4]{};
    test.valid = CrtRestartable_mbsrtowcs(nullptr, &input, 0, &state) == 3 && input == text && test.valid;
    test.valid = CrtRestartable_mbsrtowcs(output, &input, 1, &state) == 0 && input == text && test.valid;
    test.valid = CrtRestartable_mbsrtowcs(output, &input, 4, &state) == 3 && !input && test.valid;
    test.valid = output[0] == 0xd83d && output[1] == 0xde00 && output[2] == L'A' && output[3] == 0 && test.valid;
    const wchar_t* utf16 = output;
    char encoded[6]{};
    test.valid = CrtRestartable_wcsrtombs(encoded, &utf16, 3, &state) == 0 && utf16 == output && test.valid;
    size_t written = 0;
    test.valid = CrtRestartable_wcsrtombs_s(&written, encoded, sizeof(encoded), &utf16, 5, &state) == 0 && test.valid;
    test.valid = written == sizeof(text) && utf16 == output + 3 && std::memcmp(text, encoded, sizeof(text)) == 0 && test.valid;
    utf16 = output;
    test.valid = CrtRestartable_wcsrtombs_s(&written, encoded, sizeof(encoded), &utf16, sizeof(encoded), &state) == 0 && !utf16 && test.valid;
    char sentinel = 'x';
    test.valid = CrtRestartable_wcrtomb_s(&written, &sentinel, 1, 0x20ac, &state) == ERANGE && sentinel == 'x' && test.valid;
}

void __cdecl MbC11OtherGuest(void* parameter)
{
    using namespace Win32Bridge::Bridge;
    auto& valid = *static_cast<bool*>(parameter);
    char16_t unit = 0;
    char bytes[4]{};
    valid = CrtRestartable_mbrtoc16(&unit, "X", 1, nullptr) == 1 && unit == u'X' && valid;
    valid = CrtRestartable_c16rtomb(bytes, u'X', nullptr) == 1 && bytes[0] == 'X' && valid;
}

void __cdecl MbC11Contract(void* parameter)
{
    using namespace Win32Bridge::Bridge;
    auto& test = *static_cast<MbFixtureContext*>(parameter);
    constexpr auto invalid = static_cast<size_t>(-1);
    constexpr auto incomplete = static_cast<size_t>(-2);
    constexpr auto pendingLowSurrogate = static_cast<size_t>(-3);
    const char utf8[] = "\xf0\x9f\x98\x80";
    char16_t unit = 0;
    char32_t scalar = 0;
    char bytes[4]{};
    mbstate_t explicitState{};

    test.valid = SetCrtLocale(LC_CTYPE, "Japanese_Japan.932") && test.valid;
    test.valid = CrtRestartable_mbrtoc16(&unit, utf8, 4, nullptr) == 4 && unit == 0xd83d && test.valid;
    test.valid = CrtRestartable_c16rtomb(bytes, 0xd83d, nullptr) == 0 && test.valid;
    test.valid = CrtRestartable_mbrtoc32(&scalar, utf8, 2, nullptr) == incomplete && test.valid;
    test.valid = RunMbFixture(*test.other, &MbC11OtherGuest, &test.valid) && test.valid;
    test.valid = CrtRestartable_mbrtoc32(&scalar, "A", 1, &explicitState) == 1 && scalar == U'A' && test.valid;
    test.valid = CrtRestartable_mbrtoc32(&scalar, utf8 + 2, 2, nullptr) == 2 && scalar == 0x1f600 && test.valid;
    test.valid = CrtRestartable_mbrtoc16(&unit, "X", 1, nullptr) == pendingLowSurrogate && unit == 0xde00 && test.valid;
    test.valid = CrtRestartable_mbrtoc16(&unit, "X", 1, nullptr) == 1 && unit == u'X' && test.valid;
    test.valid = CrtRestartable_c16rtomb(bytes, 0xde00, nullptr) == 4 && std::memcmp(bytes, utf8, 4) == 0 && test.valid;
    test.valid = CrtRestartable_c32rtomb(bytes, 0x1f600, &explicitState) == 4 && std::memcmp(bytes, utf8, 4) == 0 && test.valid;
    test.valid = CrtRestartable_c32rtomb(bytes, 0xd800, &explicitState) == invalid && errno == EILSEQ && test.valid;
    test.valid = CrtRestartable_mbrtoc32(nullptr, utf8, 1, &explicitState) == incomplete && test.valid;
    test.valid = CrtRestartable_mbrtoc32(nullptr, "A", 1, &explicitState) == invalid && errno == EILSEQ && test.valid;
    test.valid = CrtRestartable_mbsinit(&explicitState) && test.valid;
    test.valid = CrtRestartable_c16rtomb(bytes, 0xdc00, nullptr) == invalid && errno == EILSEQ && test.valid;
    test.valid = CrtRestartable_c16rtomb(bytes, u'Z', nullptr) == 1 && bytes[0] == 'Z' && test.valid;
}

bool ValidateCrtMultibyteFixtures(){
    using namespace Win32Bridge::Bridge;
    CrtLocalePolicy model;CrtLocaleThreadState first,second;model.InitializeThread(first);model.InitializeThread(second);
    const auto before=model.MultibyteSnapshot(first);auto replacement=std::make_shared<const int>(932);
    if(!model.TryCommitMultibyte(first,before,replacement)||model.TryCommitMultibyte(second,before,std::make_shared<const int>(936))||*model.MultibyteSnapshot(second)!=932)return false;
    bool valid=false;
    // A dedicated native host thread lets the fixture choose a distinct host
    // SBCS page without attempting to restore it from ambiguous _getmbcp()==0.
    std::thread worker([&]{
        ::_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);if(!::_wsetlocale(LC_ALL,L"C")||::_setmbcp(1251)!=0)return;
        const auto host=::_get_current_locale();if(!host)return;
        MbProcessState process,otherProcess;MbThreadState state,other;state.process=&process;other.process=&otherProcess;
        MbFixtureContext test{true,&process,&other};errno=891;
        valid=RunMbFixture(state,&MbRootContract,&test)&&test.valid&&errno==891;
        MbReentryContext reentry;valid=RunMbFixture(state,&MbReentryContract,&reentry)&&reentry.valid&&reentry.original.expired()&&valid;
        MbProcessState forcedProcess;MbThreadState forcedThread;forcedThread.process=&forcedProcess;
        MbFixtureContext forced{true,&forcedProcess,nullptr};valid=RunMbFixture(forcedThread,&MbForcedRoot,&forced)&&forced.valid&&valid;
        MbProcessState conversionProcess, conversionOther;
        MbThreadState conversionThread, conversionOtherThread;
        conversionThread.process = &conversionProcess;
        conversionOtherThread.process = &conversionOther;
        MbFixtureContext conversion{true, &conversionProcess, &conversionOtherThread};
        valid = RunMbFixture(conversionThread, &MbRestartableContract, &conversion) && conversion.valid && valid;
        valid = RunMbFixture(conversionThread, &MbC11Contract, &conversion) && conversion.valid && valid;
        const auto after=::_get_current_locale();valid=after&&after->locinfo==host->locinfo&&after->mbcinfo==host->mbcinfo&&::_mbctoupper(0xFF)==0xDF&&valid;
        if(after)::_free_locale(after);::_free_locale(host);
        ImportedSymbol symbol{};symbol.library=L"ucrtbase.dll";symbol.name=L"_ismbblead";
        valid=ResolveCrtDataImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&CrtMb_ismbblead)&&valid;
        symbol.name=L"_mbsncpy_s_l";valid=ResolveCrtDataImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&CrtMb_mbsncpy_s_l)&&valid;
        symbol.name=L"mbrtowc";
        valid=ResolveCrtDataImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&CrtRestartable_mbrtowc)&&valid;
        symbol.name=L"mbrtoc16";
        valid=ResolveCrtDataImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&CrtRestartable_mbrtoc16)&&valid;
        symbol.name=L"c32rtomb";
        valid=ResolveCrtDataImport(symbol).targetAddress==reinterpret_cast<ULONGLONG>(&CrtRestartable_c32rtomb)&&valid;
    });worker.join();return valid;
}
}
