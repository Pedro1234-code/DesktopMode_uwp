#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <typeinfo>
namespace {
struct FixtureRttiRoot {virtual ~FixtureRttiRoot()=default;int root=1;};
struct FixtureRttiLeft:virtual FixtureRttiRoot {int left=2;};
struct FixtureRttiRight:virtual FixtureRttiRoot {int right=3;};
struct FixtureRttiDerived:FixtureRttiLeft,FixtureRttiRight {int derived=4;};
struct FixtureRttiOther {virtual ~FixtureRttiOther()=default;};
bool ValidateCrtRttiFixtures(){
    using namespace Win32Bridge::Bridge;
    using Cast=void*(__cdecl*)(void*,long,void*,void*,int);using Unary=void*(__cdecl*)(void*);
    auto cast=reinterpret_cast<Cast>(CrtExceptionHelperAddress(L"__RTDynamicCast"));
    auto identify=reinterpret_cast<Unary>(CrtExceptionHelperAddress(L"__RTtypeid"));
    auto complete=reinterpret_cast<Unary>(CrtExceptionHelperAddress(L"__RTCastToVoid"));
    if(!cast||!identify||!complete)return false;
    FixtureRttiDerived object;FixtureRttiLeft* left=&object;FixtureRttiRight* right=&object;FixtureRttiRoot* root=&object;
    auto type=[](const std::type_info& info){return const_cast<std::type_info*>(&info);};
    if(cast(left,0,type(typeid(FixtureRttiLeft)),type(typeid(FixtureRttiRight)),0)!=right)return false;
    if(cast(root,0,type(typeid(FixtureRttiRoot)),type(typeid(FixtureRttiDerived)),0)!=&object)return false;
    if(identify(right)!=type(typeid(FixtureRttiDerived))||complete(right)!=&object)return false;
    if(cast(nullptr,0,type(typeid(FixtureRttiLeft)),type(typeid(FixtureRttiRight)),0))return false;
    if(cast(left,0,type(typeid(FixtureRttiLeft)),type(typeid(FixtureRttiOther)),0))return false;
    try{cast(left,0,type(typeid(FixtureRttiLeft)),type(typeid(FixtureRttiOther)),1);return false;}catch(const std::bad_cast&){}
    try{identify(nullptr);return false;}catch(const std::bad_typeid&){}
    return true;
}
}
