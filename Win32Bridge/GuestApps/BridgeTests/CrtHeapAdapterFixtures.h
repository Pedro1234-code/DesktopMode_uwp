#pragma once
#include "Bridge/CRT/CrtProcessState.h"
#include "Bridge/CompatibilityCatalog.h"
#include <malloc.h>
#include <cstring>
namespace {
// Component-test context injection. Compile the actual adapter implementation;
// no allocator/control-flow replica is used by these fixtures.
Win32Bridge::Bridge::CrtProcessState* g_adapterCrt=nullptr;
Win32Bridge::Bridge::CrtProcessState* CurrentCrt(){return g_adapterCrt;}
}
#include "Bridge/CRT/CrtHeapShims.inl"
namespace {
int g_adapterInvalidCalls=0;
void __cdecl AdapterInvalidHandler(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){++g_adapterInvalidCalls;}
bool ValidateCrtHeapAdapterFixtures(){
    using namespace Win32Bridge::Bridge;
    CrtProcessState process;CrtThreadExceptionState thread;thread.process=&process.exceptions;
    const auto previous=SetCrtThreadExceptionState(&thread);g_adapterCrt=&process;
    SetCrtThreadInvalidParameterHandler(&AdapterInvalidHandler);g_adapterInvalidCalls=0;
    void* memory=CrtAlignedOffsetMalloc(64,64,3);bool valid=memory&&(reinterpret_cast<uintptr_t>(memory)+3)%64==0;
    if(memory){
        std::memset(memory,0x4b,64);const size_t oldSize=::_aligned_msize(memory,64,3);
        void* grown=CrtAlignedOffsetRecalloc(memory,1,oldSize+64,64,3);
        valid=valid&&grown!=nullptr;
        if(grown){memory=grown;
            for(size_t i=0;i<64;++i)valid=valid&&static_cast<BYTE*>(memory)[i]==0x4b&&static_cast<BYTE*>(memory)[oldSize+i]==0;
        }
        void* failed=CrtAlignedOffsetRealloc(memory,(std::numeric_limits<size_t>::max)(),64,3);
        valid=valid&&!failed&&errno==ENOMEM;
        // The previous block remains alive after invalid alignment as well.
        failed=CrtAlignedOffsetRealloc(memory,64,3,0);
        valid=valid&&!failed&&errno==EINVAL&&g_adapterInvalidCalls==1&&static_cast<BYTE*>(memory)[0]==0x4b;
        const auto freed=CrtAlignedOffsetRealloc(memory,0,3,999);valid=valid&&freed==nullptr;memory=nullptr;
    }
    const auto invalidMode=CrtSetNewMode(2);valid=valid&&invalidMode==-1&&errno==EINVAL&&g_adapterInvalidCalls==2;
    ImportedSymbol symbol{};symbol.library=L"ucrtbase.dll";symbol.name=L"_aligned_offset_malloc";
    auto resolved=ResolveCrtHeapImport(symbol);valid=valid&&resolved.targetAddress==reinterpret_cast<ULONGLONG>(&CrtAlignedOffsetMalloc);
    symbol.library=L"user32.dll";valid=valid&&!ResolveCrtHeapImport(symbol).targetAddress;
    g_adapterCrt=nullptr;SetCrtThreadExceptionState(previous);return valid;
}
}
