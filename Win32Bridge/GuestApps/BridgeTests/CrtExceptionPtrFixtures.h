#pragma once
#include "CrtExceptionFixtures.h"
#include <exception>
#include <thread>
#include <stdexcept>
namespace {
void __cdecl CaptureCurrentExceptionFixture(void* output){
    *static_cast<std::exception_ptr*>(output)=std::current_exception();
}
void __cdecl NestedCurrentExceptionFixture(void* output){
    try{throw 72;}catch(int){Win32Bridge::Bridge::InvokeCrtExecution(&CaptureCurrentExceptionFixture,output);}
}
bool ValidateCrtExceptionIsolationFixtures(){
    using namespace Win32Bridge::Bridge;
    std::exception_ptr captured;bool restored=false;
    try{try{throw 93;}catch(int){
        if(!InvokeCrtExecution(&CaptureCurrentExceptionFixture,&captured)||captured)return false;
        throw;
    }}catch(int value){restored=value==93;}
    if(!restored||!InvokeCrtExecution(&NestedCurrentExceptionFixture,&captured)||!captured)return false;
    try{std::rethrow_exception(captured);}catch(int value){return value==72;}
    return false;
}
bool ValidateCrtExceptionPtrFixtures(){
    using namespace Win32Bridge::Bridge;
    static_assert(sizeof(std::exception_ptr)==2*sizeof(void*),"MSVC exception_ptr ABI");
    const wchar_t* names[]={L"__ExceptionPtrCreate",L"__ExceptionPtrDestroy",L"__ExceptionPtrCopy",L"__ExceptionPtrAssign",L"__ExceptionPtrCompare",L"__ExceptionPtrToBool",L"__ExceptionPtrSwap",L"__ExceptionPtrCurrentException",L"__ExceptionPtrRethrow",L"__ExceptionPtrCopyException"};
    for(auto name:names)if(!CrtExceptionPtrHelperAddress(name))return false;
    if(CrtExceptionPtrHelperAddress(L"not_an_exception_ptr_export"))return false;
    using Capture=void(__cdecl*)(void*);using CopyException=void(__cdecl*)(void*,const void*,const void*);
    auto capture=reinterpret_cast<Capture>(CrtExceptionPtrHelperAddress(L"__ExceptionPtrCurrentException"));
    auto copyException=reinterpret_cast<CopyException>(CrtExceptionPtrHelperAddress(L"__ExceptionPtrCopyException"));
    auto rethrow=reinterpret_cast<void(__cdecl*)(const void*)>(CrtExceptionPtrHelperAddress(L"__ExceptionPtrRethrow"));
    std::exception_ptr empty;capture(&empty);if(empty)return false;
    ObservedThrowMetadata metadata;
    try{ObserveNativeThrow(&ThrowFixtureInteger,nullptr,&metadata);}catch(int){}
    if(!metadata.info||!metadata.base)return false;
    // Force the manual-image path even though this fixture is a native image.
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(metadata.base);
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(metadata.base+dos->e_lfanew);
    if(!RegisterCrtGuestImage(reinterpret_cast<void*>(metadata.base),nt->OptionalHeader.SizeOfImage))return false;
    std::exception_ptr captured;int value=345;
    copyException(&captured,&value,metadata.info);
    UnregisterCrtGuestImage(reinterpret_cast<void*>(metadata.base));
    if(!captured)return false;
    value=999;bool valid=false;
    try{rethrow(&captured);}catch(int result){valid=result==345;}
    std::exception_ptr duplicate=captured;
    bool caughtOnThread=false;
    std::thread worker([&]{try{rethrow(&duplicate);}catch(int result){caughtOnThread=result==345;}});worker.join();
    if(!valid||!caughtOnThread||duplicate!=captured)return false;
    try{rethrow(&empty);return false;}catch(const std::bad_exception&){}
    // Capture a nontrivial derived exception and retain its native lifetime.
    std::exception_ptr object;
    try{throw std::runtime_error("retained guest error");}catch(...){capture(&object);}
    if(!object)return false;
    try{rethrow(&object);}catch(const std::exception& error){return std::strcmp(error.what(),"retained guest error")==0;}
    return false;
}
}
