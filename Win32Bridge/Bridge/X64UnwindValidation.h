#pragma once
#include <windows.h>
#include <cstring>
#include <cstddef>
namespace Win32Bridge { namespace Bridge {
// Structural validation before handing guest .pdata/.xdata to the native
// unwinder. Instruction-level semantics remain the platform unwinder's job.
inline bool ReadGuestUnwindBytes(const BYTE* image,size_t size,size_t rva,void* output,size_t bytes){
    if(rva>size||bytes>size-rva)return false;
    __try{std::memcpy(output,image+rva,bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline bool ValidateGuestX64Unwind(const BYTE* image,size_t size,RUNTIME_FUNCTION function){
    if(!image)return false;
    DWORD visited[UNWIND_CHAIN_LIMIT]{};unsigned count=0;
    for(;;){
        if(function.BeginAddress>=function.EndAddress||function.EndAddress>size)return false;
        const DWORD raw=function.UnwindData;
        for(unsigned i=0;i<count;++i)if(visited[i]==raw)return false;
        if(count==UNWIND_CHAIN_LIMIT)return false;visited[count++]=raw;
        if(raw&RUNTIME_FUNCTION_INDIRECT){
            const DWORD target=raw&~static_cast<DWORD>(RUNTIME_FUNCTION_INDIRECT);
            if(target%4||!ReadGuestUnwindBytes(image,size,target,&function,sizeof(function)))return false;
            continue;
        }
        if(raw%4)return false;
        BYTE header[4]{};if(!ReadGuestUnwindBytes(image,size,raw,header,sizeof(header)))return false;
        const unsigned version=header[0]&7,flags=header[0]>>3;
        if((version!=1&&version!=2)||(flags&~7u)||((flags&4u)&&(flags&3u)))return false;
        const size_t codes=static_cast<size_t>(header[2])*2;
        BYTE scratch[510];if(codes&&!ReadGuestUnwindBytes(image,size,static_cast<size_t>(raw)+4,scratch,codes))return false;
        const size_t tail=static_cast<size_t>(raw)+((4+codes+3)&~static_cast<size_t>(3));
        if(flags&4u){if(!ReadGuestUnwindBytes(image,size,tail,&function,sizeof(function)))return false;continue;}
        if(flags&3u){DWORD handler=0;if(!ReadGuestUnwindBytes(image,size,tail,&handler,sizeof(handler))||!handler||handler>=size)return false;}
        return true;
    }
}
} }
