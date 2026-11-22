#pragma once
#include "Bridge/CRT/CrtHeapPolicy.h"
#include <cerrno>
#include <stdexcept>
namespace {
Win32Bridge::Bridge::CrtHeapPolicy* g_heapPolicy=nullptr;
unsigned g_heapHandlerCalls=0;
size_t g_heapHandlerSize=0;
int __cdecl FixtureHeapRecover(size_t size){++g_heapHandlerCalls;g_heapHandlerSize=size;g_heapPolicy->SetMode(1);return 7;}
int __cdecl FixtureHeapReject(size_t){++g_heapHandlerCalls;return 0;}
int __cdecl FixtureHeapThrow(size_t){throw std::runtime_error("fixture new-handler");}
bool ValidateCrtHeapFixtures(){
    using Win32Bridge::Bridge::CrtHeapPolicy;
    CrtHeapPolicy first,second;g_heapPolicy=&first;
    if(first.Mode()!=0||first.GetHandler()||first.CallHandler(20))return false;
    if(first.SetHandler(&FixtureHeapRecover)||first.GetHandler()!=&FixtureHeapRecover)return false;
    unsigned allocations=0;g_heapHandlerCalls=0;
    if(first.Retry(20,[&](size_t)->void*{++allocations;return nullptr;})||allocations!=1||g_heapHandlerCalls)return false;
    if(first.SetMode(1)!=0||first.Mode()!=1||second.Mode()!=0||second.GetHandler())return false;
    allocations=0;int sentinel=0;
    auto result=first.Retry(0,[&](size_t size)->void*{if(size!=1)throw std::logic_error("zero allocation size");return ++allocations==3?&sentinel:nullptr;});
    if(result!=&sentinel||allocations!=3||g_heapHandlerCalls!=2||g_heapHandlerSize!=1)return false;
    if(first.SetHandler(&FixtureHeapReject)!=&FixtureHeapRecover)return false;
    allocations=0;
    if(first.Retry(8,[&](size_t)->void*{++allocations;return nullptr;})||allocations!=1||g_heapHandlerCalls!=3||errno!=ENOMEM)return false;
    first.SetHandler(&FixtureHeapRecover);allocations=0;g_heapHandlerCalls=0;
    if(first.Retry(CrtHeapPolicy::MaximumRequest+1,[&](size_t)->void*{++allocations;return &sentinel;})||allocations||g_heapHandlerCalls||errno!=ENOMEM)return false;
    size_t total=99;
    if(CrtHeapPolicy::Product(2,CrtHeapPolicy::MaximumRequest,&total)||errno!=ENOMEM)return false;
    if(!CrtHeapPolicy::Product(0,(std::numeric_limits<size_t>::max)(),&total)||total)return false;
    if(!CrtHeapPolicy::Product(4,8,&total)||total!=32)return false;
    first.SetHandler(&FixtureHeapThrow);
    try{first.CallHandler(32);return false;}catch(const std::runtime_error&){}
    if(first.SetHandler(nullptr)!=&FixtureHeapThrow||first.CallHandler(1))return false;
    if(first.SetMode(2)!=-1||errno!=EINVAL||first.Mode()!=1)return false;
    size_t request=0;
    if(CrtHeapPolicy::AlignedRequest(32,16,0,&request)||request!=55)return false;
    if(CrtHeapPolicy::AlignedRequest(32,64,3,&request)||request!=108)return false;
    if(CrtHeapPolicy::AlignedRequest(0,16,0,&request)||request!=23)return false;
    if(CrtHeapPolicy::AlignedRequest(16,0,0,&request)!=EINVAL)return false;
    if(CrtHeapPolicy::AlignedRequest(16,3,0,&request)!=EINVAL)return false;
    if(CrtHeapPolicy::AlignedRequest(16,16,16,&request)!=EINVAL)return false;
    if(CrtHeapPolicy::AlignedRequest(CrtHeapPolicy::MaximumRequest,16,0,&request)!=ENOMEM)return false;
    if(CrtHeapPolicy::AlignedRequest(32,static_cast<size_t>(1)<<(sizeof(size_t)*8-1),0,&request))return false;
    g_heapPolicy=nullptr;return true;
}
}
