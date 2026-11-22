#pragma once
#include "Bridge/X64UnwindValidation.h"
#include <array>
namespace {
bool ValidateX64UnwindFixtures(){
    using namespace Win32Bridge::Bridge;
    std::array<BYTE,4096> image{};RUNTIME_FUNCTION function{0x100,0x120,0x200};
    auto put=[&](size_t offset,const void* data,size_t size){std::memcpy(image.data()+offset,data,size);};
    image[0x200]=1;if(!ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    image[0x200]=2;if(!ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    image[0x200]=3;if(ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    image[0x200]=1;image[0x202]=255;if(ValidateGuestX64Unwind(image.data(),0x208,function))return false;
    image[0x202]=0;image[0x200]=9;DWORD handler=0x140;put(0x204,&handler,4);
    if(!ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    handler=0x1000;put(0x204,&handler,4);if(ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    image[0x200]=33;RUNTIME_FUNCTION chained{0x100,0x140,0x300};put(0x204,&chained,sizeof(chained));image[0x300]=1;
    if(!ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    chained.UnwindData=0x200;put(0x204,&chained,sizeof(chained));if(ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    image[0x200]=41;if(ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    function.UnwindData=0x201;chained.UnwindData=0x300;put(0x200,&chained,sizeof(chained));
    if(!ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    chained.UnwindData=0x201;put(0x200,&chained,sizeof(chained));if(ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    function.UnwindData=0x203;if(ValidateGuestX64Unwind(image.data(),image.size(),function))return false;
    return true;
}
}
