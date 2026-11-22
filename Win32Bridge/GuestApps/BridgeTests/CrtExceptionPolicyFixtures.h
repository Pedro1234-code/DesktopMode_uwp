#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <cstdlib>
#include <new.h>
namespace {
int g_invalidGlobal=0,g_invalidLocal=0,g_invalidHost=0,g_purecall=0;
void __cdecl FixtureInvalidGlobal(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){++g_invalidGlobal;}
void __cdecl FixtureInvalidLocal(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){++g_invalidLocal;}
void __cdecl FixtureInvalidHost(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){++g_invalidHost;}
void __cdecl FixturePurecall(){++g_purecall;}
void __cdecl FixtureTranslator(unsigned,EXCEPTION_POINTERS*){}
void __cdecl FixtureGuestTranslator(unsigned,EXCEPTION_POINTERS*){}
void __cdecl InvokeInvalidFixture(void*){::_set_new_mode(2);}
void __cdecl InvokePurecallFixture(void*){
    auto function=reinterpret_cast<int(__cdecl*)()>(Win32Bridge::Bridge::CrtExceptionHelperAddress(L"_purecall"));function();
}
void __cdecl CheckGuestTranslatorFixture(void* output){
    using namespace Win32Bridge::Bridge;
    auto previous=SetCrtSeTranslator(nullptr);*static_cast<bool*>(output)=previous==&FixtureGuestTranslator;SetCrtSeTranslator(previous);
}
#pragma warning(push)
#pragma warning(disable:4535)
bool ValidateCrtExceptionPolicyFixtures(){
    using namespace Win32Bridge::Bridge;
    CrtExceptionProcessState process,otherProcess;
    CrtThreadExceptionState first,second,other;first.process=&process;second.process=&process;other.process=&otherProcess;
    first.translator=&FixtureGuestTranslator;
    const auto hostInvalid=::_set_thread_local_invalid_parameter_handler(&FixtureInvalidHost);
    const auto hostTranslator=::_set_se_translator(&FixtureTranslator);
    const auto old=SetCrtThreadExceptionState(&first);
    g_invalidGlobal=g_invalidLocal=g_invalidHost=g_purecall=0;
    bool valid=!SetCrtInvalidParameterHandler(&FixtureInvalidGlobal)&&!SetCrtThreadInvalidParameterHandler(&FixtureInvalidLocal);
    valid=InvokeCrtExecution(&InvokeInvalidFixture,nullptr)&&valid&&g_invalidLocal==1&&g_invalidGlobal==0;
    SetCrtThreadExceptionState(&second);
    valid=InvokeCrtExecution(&InvokeInvalidFixture,nullptr)&&valid&&g_invalidGlobal==1&&GetCrtThreadInvalidParameterHandler()==nullptr;
    SetCrtThreadExceptionState(&other);
    valid=!InvokeCrtExecution(&InvokeInvalidFixture,nullptr)&&valid&&g_invalidGlobal==1;
    SetCrtThreadExceptionState(&first);
    bool translator=false;valid=InvokeCrtExecution(&CheckGuestTranslatorFixture,&translator)&&translator&&valid;
    valid=::_get_thread_local_invalid_parameter_handler()==&FixtureInvalidHost&&valid;
    const auto restoredTranslator=::_set_se_translator(&FixtureTranslator);valid=restoredTranslator==&FixtureTranslator&&valid;
    valid=!SetCrtPurecallHandler(&FixturePurecall)&&valid;
    valid=!InvokeCrtExecution(&InvokePurecallFixture,nullptr)&&valid&&g_purecall==1;
    SetCrtThreadExceptionState(&second);valid=GetCrtPurecallHandler()==&FixturePurecall&&valid;
    SetCrtThreadExceptionState(&other);valid=GetCrtPurecallHandler()==nullptr&&GetCrtInvalidParameterHandler()==nullptr&&valid;
    valid=g_invalidHost==0&&valid;
    SetCrtThreadExceptionState(old);::_set_thread_local_invalid_parameter_handler(hostInvalid);::_set_se_translator(hostTranslator);
    return valid;
}
#pragma warning(pop)
}
