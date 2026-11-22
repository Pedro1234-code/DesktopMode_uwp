// Included inside Win32Bridge::Bridge by CrtExceptions.cpp.
namespace {
void __cdecl GuestExceptionPtrCopyException(void* destination,const void* object,const void* metadata) noexcept {
    auto info=static_cast<const ThrowInfo*>(metadata);
    if(!info||!CrtGuestImageBase(info)){::__ExceptionPtrCopyException(destination,object,metadata);return;}
    // make_exception_ptr normally discovers the throw image through the host
    // loader. A manually mapped PE has no host loader entry. Populate the
    // existing x64 ABI record and use the native current_exception copier,
    // which already consumes the fourth parameter (the real throw image base).
    if(info->attributes&TI_IsWinRT){auto preparation=(*static_cast<WinRtThrowPreparation** const*>(object))[-1];info=preparation->throwInfo;}
    const uintptr_t base=info?CrtGuestImageBase(info):0;
    if(!base){::__ExceptionPtrCopyException(destination,object,metadata);return;}
    EXCEPTION_RECORD record{};
    record.ExceptionCode=EH_EXCEPTION_NUMBER;record.ExceptionFlags=EXCEPTION_NONCONTINUABLE;record.NumberParameters=4;
    record.ExceptionInformation[0]=(info->attributes&TI_IsPure)?EH_PURE_MAGIC_NUMBER1:EH_MAGIC_NUMBER1;
    record.ExceptionInformation[1]=reinterpret_cast<ULONG_PTR>(object);
    record.ExceptionInformation[2]=reinterpret_cast<ULONG_PTR>(info);
    record.ExceptionInformation[3]=base;
    auto slot=::__current_exception();void* previous=*slot;
    __try {*slot=&record;::__ExceptionPtrCurrentException(destination);}
    __finally {*slot=previous;}
}
}
uintptr_t CrtExceptionPtrHelperAddress(const wchar_t* name){
    if(!name)return 0;
#define CRT_PTR_EXPORT(plain,decorated,function) if(!std::wcscmp(name,L##plain)||!std::wcscmp(name,L##decorated))return reinterpret_cast<uintptr_t>(&function)
    CRT_PTR_EXPORT("__ExceptionPtrCreate","?__ExceptionPtrCreate@@YAXPEAX@Z",::__ExceptionPtrCreate);
    CRT_PTR_EXPORT("__ExceptionPtrDestroy","?__ExceptionPtrDestroy@@YAXPEAX@Z",::__ExceptionPtrDestroy);
    CRT_PTR_EXPORT("__ExceptionPtrCopy","?__ExceptionPtrCopy@@YAXPEAXPEBX@Z",::__ExceptionPtrCopy);
    CRT_PTR_EXPORT("__ExceptionPtrAssign","?__ExceptionPtrAssign@@YAXPEAXPEBX@Z",::__ExceptionPtrAssign);
    CRT_PTR_EXPORT("__ExceptionPtrCompare","?__ExceptionPtrCompare@@YA_NPEBX0@Z",::__ExceptionPtrCompare);
    CRT_PTR_EXPORT("__ExceptionPtrToBool","?__ExceptionPtrToBool@@YA_NPEBX@Z",::__ExceptionPtrToBool);
    CRT_PTR_EXPORT("__ExceptionPtrSwap","?__ExceptionPtrSwap@@YAXPEAX0@Z",::__ExceptionPtrSwap);
    CRT_PTR_EXPORT("__ExceptionPtrCurrentException","?__ExceptionPtrCurrentException@@YAXPEAX@Z",::__ExceptionPtrCurrentException);
    CRT_PTR_EXPORT("__ExceptionPtrRethrow","?__ExceptionPtrRethrow@@YAXPEBX@Z",::__ExceptionPtrRethrow);
    CRT_PTR_EXPORT("__ExceptionPtrCopyException","?__ExceptionPtrCopyException@@YAXPEAXPEBX1@Z",GuestExceptionPtrCopyException);
#undef CRT_PTR_EXPORT
    return 0;
}
