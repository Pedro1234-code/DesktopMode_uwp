namespace {
using GuestNewHandler=Win32Bridge::Bridge::CrtHeapPolicy::Handler;
int __cdecl CrtSetNewMode(int mode){auto state=CurrentCrt();if(!state){errno=EINVAL;return -1;}if(mode!=0&&mode!=1){Win32Bridge::Bridge::ReportCrtInvalidParameter(L"mode == 0 || mode == 1",L"_set_new_mode",nullptr,0,0);errno=EINVAL;return -1;}return state->heap.SetMode(mode);}
int __cdecl CrtQueryNewMode(){auto state=CurrentCrt();return state?state->heap.Mode():0;}
GuestNewHandler __cdecl CrtSetNewHandler(GuestNewHandler handler){auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}return state->heap.SetHandler(handler);}
GuestNewHandler __cdecl CrtQueryNewHandler(){auto state=CurrentCrt();return state?state->heap.GetHandler():nullptr;}
int __cdecl CrtCallNewHandler(size_t size){auto state=CurrentCrt();return state?state->heap.CallHandler(size):0;}
void* __cdecl CrtMalloc(size_t size){
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    return state->heap.Retry(size,[](size_t actual){return ::malloc(actual);});
}
void* __cdecl CrtCalloc(size_t count,size_t size){
    using Win32Bridge::Bridge::CrtHeapPolicy;
    size_t total=0;if(!CrtHeapPolicy::Product(count,size,&total))return nullptr;
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    return state->heap.Retry(total,[](size_t actual){return ::calloc(1,actual);});
}
void* __cdecl CrtRealloc(void* memory,size_t size){
    if(!memory)return CrtMalloc(size);
    if(!size){::free(memory);return nullptr;}
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    return state->heap.Retry(size,[memory](size_t actual){return ::realloc(memory,actual);});
}
void* __cdecl CrtRecalloc(void* memory,size_t count,size_t size){
    using Win32Bridge::Bridge::CrtHeapPolicy;
    size_t total=0;if(!CrtHeapPolicy::Product(count,size,&total))return nullptr;
    if(!memory)return CrtCalloc(count,size);
    if(!total){::free(memory);return nullptr;}
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    // Native recalloc retains the physical old allocation size and zeros only
    // its newly added tail, rather than guessing the caller's requested size.
    return state->heap.Retry(total,[memory](size_t actual){return ::_recalloc(memory,1,actual);});
}
bool CrtAlignedRequest(size_t size,size_t alignment,size_t offset,size_t* request){
    const int status=Win32Bridge::Bridge::CrtHeapPolicy::AlignedRequest(size,alignment,offset,request);
    if(status==EINVAL)Win32Bridge::Bridge::ReportCrtInvalidParameter(L"power-of-two alignment and offset < size",L"aligned allocation",nullptr,0,0);
    if(status){errno=status;return false;}return true;
}
void* __cdecl CrtAlignedOffsetMalloc(size_t size,size_t alignment,size_t offset){
    size_t request=0;if(!CrtAlignedRequest(size,alignment,offset,&request))return nullptr;
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    return state->heap.Retry(request,[=](size_t){return ::_aligned_offset_malloc(size,alignment,offset);});
}
void* __cdecl CrtAlignedMalloc(size_t size,size_t alignment){return CrtAlignedOffsetMalloc(size,alignment,0);}
void* __cdecl CrtAlignedOffsetRealloc(void* memory,size_t size,size_t alignment,size_t offset){
    if(!memory)return CrtAlignedOffsetMalloc(size,alignment,offset);
    if(!size){::_aligned_free(memory);return nullptr;}
    size_t request=0;if(!CrtAlignedRequest(size,alignment,offset,&request))return nullptr;
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    return state->heap.Retry(request,[=](size_t){return ::_aligned_offset_realloc(memory,size,alignment,offset);});
}
void* __cdecl CrtAlignedRealloc(void* memory,size_t size,size_t alignment){return CrtAlignedOffsetRealloc(memory,size,alignment,0);}
void* __cdecl CrtAlignedOffsetRecalloc(void* memory,size_t count,size_t size,size_t alignment,size_t offset){
    size_t total=0;if(!Win32Bridge::Bridge::CrtHeapPolicy::Product(count,size,&total))return nullptr;
    if(memory&&!total){::_aligned_free(memory);return nullptr;}
    size_t request=0;if(!CrtAlignedRequest(total,alignment,offset,&request))return nullptr;
    auto state=CurrentCrt();if(!state){errno=EINVAL;return nullptr;}
    return state->heap.Retry(request,[=](size_t){return ::_aligned_offset_recalloc(memory,count,size,alignment,offset);});
}
void* __cdecl CrtAlignedRecalloc(void* memory,size_t count,size_t size,size_t alignment){return CrtAlignedOffsetRecalloc(memory,count,size,alignment,0);}
Win32Bridge::Bridge::ImportResolution ResolveCrtHeapImport(const Win32Bridge::Bridge::ImportedSymbol& symbol){
    using namespace Win32Bridge::Bridge;
    ImportResolution result{ImportDisposition::Unsupported,L"No guest heap adapter.",0};
    if(symbol.importedByOrdinal||(_wcsicmp(symbol.library.c_str(),L"msvcrt.dll")&&_wcsicmp(symbol.library.c_str(),L"ucrtbase.dll")))return result;
#define CRT_HEAP(exportName,function) if(symbol.name==L##exportName)result.targetAddress=reinterpret_cast<ULONGLONG>(&function);else
    CRT_HEAP("malloc",CrtMalloc)
    CRT_HEAP("_malloc_base",CrtMalloc)
    CRT_HEAP("calloc",CrtCalloc)
    CRT_HEAP("_calloc_base",CrtCalloc)
    CRT_HEAP("realloc",CrtRealloc)
    CRT_HEAP("_realloc_base",CrtRealloc)
    CRT_HEAP("_recalloc",CrtRecalloc)
    CRT_HEAP("_recalloc_base",CrtRecalloc)
    CRT_HEAP("_aligned_malloc",CrtAlignedMalloc)
    CRT_HEAP("_aligned_offset_malloc",CrtAlignedOffsetMalloc)
    CRT_HEAP("_aligned_realloc",CrtAlignedRealloc)
    CRT_HEAP("_aligned_offset_realloc",CrtAlignedOffsetRealloc)
    CRT_HEAP("_aligned_recalloc",CrtAlignedRecalloc)
    CRT_HEAP("_aligned_offset_recalloc",CrtAlignedOffsetRecalloc)
    CRT_HEAP("_set_new_mode",CrtSetNewMode)
    CRT_HEAP("_query_new_mode",CrtQueryNewMode)
    CRT_HEAP("_set_new_handler",CrtSetNewHandler)
    CRT_HEAP("_query_new_handler",CrtQueryNewHandler)
    CRT_HEAP("_callnewh",CrtCallNewHandler)
    {}
#undef CRT_HEAP
    if(result.targetAddress){result.disposition=ImportDisposition::NeedsBridge;result.note=L"CRT allocation with per-guest new-handler state.";}
    return result;
}
}
