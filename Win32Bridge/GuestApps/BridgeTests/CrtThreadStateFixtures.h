#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <cerrno>
#include <cstdlib>
#if defined(_M_X64)
#include <xmmintrin.h>
#endif
namespace {
struct CrtThreadStateFixtureContext {
    Win32Bridge::Bridge::CrtThreadExceptionState* first;
    Win32Bridge::Bridge::CrtThreadExceptionState* second;
    bool valid=true;
};
void __cdecl InitialThreadErrorsFixture(void* context){
    auto result=static_cast<bool*>(context);unsigned long dos=0;::_get_doserrno(&dos);
    *result=errno==0&&dos==0;errno=EACCES;::_set_doserrno(5);
}
void __cdecl NestedThreadErrorsFixture(void* context){
    auto result=static_cast<bool*>(context);unsigned long dos=0;::_get_doserrno(&dos);
    *result=*result&&errno==ERANGE&&dos==122;errno=ENOENT;::_set_doserrno(2);
}
void __cdecl ParentThreadErrorsFixture(void* context){
    auto result=static_cast<bool*>(context);unsigned long dos=0;::_get_doserrno(&dos);
    *result=errno==EACCES&&dos==5;errno=ERANGE;::_set_doserrno(122);
    *result=Win32Bridge::Bridge::InvokeCrtExecution(&NestedThreadErrorsFixture,result)&&*result;
    ::_get_doserrno(&dos);*result=*result&&errno==ENOENT&&dos==2;
}
void __cdecl ReenteredFirstThreadErrorsFixture(void* context){
    auto result=static_cast<CrtThreadStateFixtureContext*>(context);unsigned long dos=0;::_get_doserrno(&dos);
    result->valid=result->valid&&errno==EEXIST&&dos==183;errno=ENOEXEC;::_set_doserrno(193);
}
void __cdecl SecondThreadErrorsFixture(void* context){
    using namespace Win32Bridge::Bridge;
    auto result=static_cast<CrtThreadStateFixtureContext*>(context);unsigned long dos=0;::_get_doserrno(&dos);
    result->valid=result->valid&&errno==0&&dos==0;errno=ENOSPC;::_set_doserrno(112);
    SetCrtThreadExceptionState(result->first);
    result->valid=InvokeCrtExecution(&ReenteredFirstThreadErrorsFixture,result)&&result->valid;
    SetCrtThreadExceptionState(result->second);
    ::_get_doserrno(&dos);result->valid=result->valid&&errno==ENOSPC&&dos==112;
}
void __cdecl CrossGuestThreadErrorsFixture(void* context){
    using namespace Win32Bridge::Bridge;
    auto result=static_cast<CrtThreadStateFixtureContext*>(context);errno=EEXIST;::_set_doserrno(183);
    SetCrtThreadExceptionState(result->second);
    result->valid=InvokeCrtExecution(&SecondThreadErrorsFixture,result)&&result->valid;
    SetCrtThreadExceptionState(result->first);
    unsigned long dos=0;::_get_doserrno(&dos);result->valid=result->valid&&errno==ENOEXEC&&dos==193;
}
void __cdecl TerminateThreadErrorsFixture(void*){errno=EIO;::_set_doserrno(123);Win32Bridge::Bridge::TerminateCrtExecution();}
#if defined(_M_X64)
void __cdecl SetGuestRoundingFixture(void* context)
{
    auto result = static_cast<bool*>(context);
    *result = (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST;
    _MM_SET_ROUNDING_MODE(_MM_ROUND_DOWN);
}

void __cdecl CheckGuestRoundingFixture(void* context)
{
    auto result = static_cast<bool*>(context);
    *result = (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_DOWN;
}
#endif
bool ValidateCrtThreadStateFixtures(){
    using namespace Win32Bridge::Bridge;
    CrtThreadExceptionState first,second;auto previous=SetCrtThreadExceptionState(&first);
    const int saved=errno;unsigned long savedDos=0;::_get_doserrno(&savedDos);
    errno=EDOM;::_set_doserrno(111);bool inner=false;
    bool valid=CurrentCrtThreadExceptionState()==&first&&InvokeCrtExecution(&InitialThreadErrorsFixture,&inner)&&inner&&first.errorNumber==EACCES&&first.dosError==5;
    unsigned long dos=0;::_get_doserrno(&dos);valid=valid&&errno==EDOM&&dos==111;
    inner=false;valid=InvokeCrtExecution(&ParentThreadErrorsFixture,&inner)&&inner&&valid;
    CrtThreadStateFixtureContext cross{&first,&second};valid=InvokeCrtExecution(&CrossGuestThreadErrorsFixture,&cross)&&cross.valid&&valid;
    valid=valid&&first.errorNumber==ENOEXEC&&first.dosError==193&&second.errorNumber==ENOSPC&&second.dosError==112;
    const bool completed=InvokeCrtExecution(&TerminateThreadErrorsFixture,nullptr);
    ::_get_doserrno(&dos);valid=!completed&&valid&&errno==EDOM&&dos==111&&first.errorNumber==EIO&&first.dosError==123;
    valid=valid&&NextCrtRandom()==41;SeedCrtRandom(17);valid=valid&&NextCrtRandom()==94;
    SetCrtThreadExceptionState(&second);valid=valid&&NextCrtRandom()==41;
    SeedCrtRandom(0xffffffffu);valid=valid&&NextCrtRandom()==35;
    SetCrtThreadExceptionState(&first);valid=valid&&NextCrtRandom()==26602;
    first=CrtThreadExceptionState{};valid=valid&&NextCrtRandom()==41;
#if defined(_M_X64)
    const unsigned int hostMxcsr = _mm_getcsr();
    _MM_SET_ROUNDING_MODE(_MM_ROUND_UP);
    inner = false;
    valid = InvokeCrtExecution(&SetGuestRoundingFixture, &inner) && inner && valid;
    valid = (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_UP && valid;
    inner = false;
    valid = InvokeCrtExecution(&CheckGuestRoundingFixture, &inner) && inner && valid;
    valid = (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_UP && valid;
    _mm_setcsr(hostMxcsr);
#endif
    SetCrtThreadExceptionState(previous);::_set_doserrno(savedDos);errno=saved;
    return valid;
}
}
