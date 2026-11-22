namespace Win32Bridge { namespace Bridge {
HANDLE WINAPI BridgeCreateEventExW(LPSECURITY_ATTRIBUTES security, LPCWSTR name, DWORD flags, DWORD access)
{
    DWORD error = ERROR_SUCCESS, inheritance = 0;
    if (flags & ~(CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    if (!ReadHandleSecurityAttributes(security, &inheritance, &error))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    const HANDLE handle = kernel->CreateEvent((flags & CREATE_EVENT_MANUAL_RESET) != 0,
        (flags & CREATE_EVENT_INITIAL_SET) != 0, name, &error, inheritance != 0, access);
    SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI BridgeCreateMutexExW(LPSECURITY_ATTRIBUTES security, LPCWSTR name, DWORD flags, DWORD access)
{
    // CREATE_MUTEX_INITIAL_OWNER can be absent from the UWP SDK partition.
    constexpr DWORD InitialOwner = 1;
    DWORD error = ERROR_SUCCESS, inheritance = 0;
    if (flags & ~InitialOwner)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    if (!ReadHandleSecurityAttributes(security, &inheritance, &error))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    const HANDLE handle = kernel->CreateMutex((flags & InitialOwner) != 0, name, &error, inheritance != 0, access);
    SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI BridgeCreateSemaphoreExW(LPSECURITY_ATTRIBUTES security, LONG initial, LONG maximum,
    LPCWSTR name, DWORD flags, DWORD access)
{
    DWORD error = ERROR_SUCCESS, inheritance = 0;
    if (flags)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    if (!ReadHandleSecurityAttributes(security, &inheritance, &error))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    const HANDLE handle = kernel->CreateSemaphore(initial, maximum, name, &error, inheritance != 0, access);
    SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI BridgeOpenMutexW(DWORD access, BOOL inherit, LPCWSTR name)
{
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = kernel->OpenMutex(access, inherit, name, &error);
    SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI BridgeOpenSemaphoreW(DWORD access, BOOL inherit, LPCWSTR name)
{
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = kernel->OpenSemaphore(access, inherit, name, &error);
    SetGuestLastError(error);
    return handle;
}

template<class Call>
HANDLE WithAnsiSynchronizationName(LPCSTR name, const Call& call)
{
    if (!name) return call(nullptr);
    try
    {
        const int length = BridgeMultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
        if (!length) return nullptr;
        std::wstring wide(static_cast<size_t>(length), L'\0');
        if (!BridgeMultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), length)) return nullptr;
        return call(wide.c_str());
    }
    catch (const std::bad_alloc&)
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HANDLE WINAPI BridgeCreateEventA(LPSECURITY_ATTRIBUTES security, BOOL manual, BOOL initial, LPCSTR name)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateEventW(security, manual, initial, wide); }); }
HANDLE WINAPI BridgeCreateEventExA(LPSECURITY_ATTRIBUTES security, LPCSTR name, DWORD flags, DWORD access)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateEventExW(security, wide, flags, access); }); }
HANDLE WINAPI BridgeOpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeOpenEventW(access, inherit, wide); }); }
HANDLE WINAPI BridgeCreateMutexA(LPSECURITY_ATTRIBUTES security, BOOL initial, LPCSTR name)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateMutexW(security, initial, wide); }); }
HANDLE WINAPI BridgeCreateMutexExA(LPSECURITY_ATTRIBUTES security, LPCSTR name, DWORD flags, DWORD access)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateMutexExW(security, wide, flags, access); }); }
HANDLE WINAPI BridgeOpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeOpenMutexW(access, inherit, wide); }); }
HANDLE WINAPI BridgeCreateSemaphoreA(LPSECURITY_ATTRIBUTES security, LONG initial, LONG maximum, LPCSTR name)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateSemaphoreW(security, initial, maximum, wide); }); }
HANDLE WINAPI BridgeCreateSemaphoreExA(LPSECURITY_ATTRIBUTES security, LONG initial, LONG maximum,
    LPCSTR name, DWORD flags, DWORD access)
{
    return WithAnsiSynchronizationName(name, [&](LPCWSTR wide)
    { return BridgeCreateSemaphoreExW(security, initial, maximum, wide, flags, access); });
}
HANDLE WINAPI BridgeOpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name)
{ return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeOpenSemaphoreW(access, inherit, wide); }); }

namespace
{
    constexpr DWORD GuestTimerManualReset = 0x1;
    constexpr DWORD GuestTimerHighResolution = 0x2;

    BOOL SetGuestWaitableTimer(HANDLE handle, const LARGE_INTEGER* dueTime, LONG period,
        GuestKernelContext::WaitableTimerApc::Procedure callback, void* parameter, ULONG tolerableDelay,
        bool resume)
    {
        const auto kernel = CurrentKernelOrFail();
        if (!kernel) return FALSE;
        if (!dueTime)
        {
            SetGuestLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        LARGE_INTEGER due{};
        if (!CopyConsoleBytes(&due, dueTime, sizeof(due)))
        {
            SetGuestLastError(ERROR_NOACCESS);
            return FALSE;
        }
        DWORD error = ERROR_SUCCESS;
        const bool armed = ArmGuestWaitableTimer(*kernel, handle, due.QuadPart, period, callback,
            parameter, tolerableDelay, error);
        if (error == ERROR_INVALID_FUNCTION) error = ERROR_INVALID_HANDLE;
        // The guest can schedule a timer but cannot resume the host from sleep.
        SetGuestLastError(armed && resume ? ERROR_NOT_SUPPORTED : error);
        return armed ? TRUE : FALSE;
    }
}

HANDLE WINAPI BridgeCreateWaitableTimerExW(LPSECURITY_ATTRIBUTES security, LPCWSTR name, DWORD flags, DWORD access)
{
    if (flags & ~(GuestTimerManualReset | GuestTimerHighResolution))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    DWORD error = ERROR_SUCCESS, inheritance = 0;
    if (!ReadHandleSecurityAttributes(security, &inheritance, &error))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    const HANDLE handle = kernel->CreateTimerObject((flags & GuestTimerManualReset) != 0, name, &error,
        inheritance != 0, access);
    SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI BridgeCreateWaitableTimerW(LPSECURITY_ATTRIBUTES security, BOOL manual, LPCWSTR name)
{
    return BridgeCreateWaitableTimerExW(security, name, manual ? GuestTimerManualReset : 0, GENERIC_ALL);
}

HANDLE WINAPI BridgeOpenWaitableTimerW(DWORD access, BOOL inherit, LPCWSTR name)
{
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = kernel->OpenTimerObject(access, inherit, name, &error);
    if (!handle) SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI BridgeCreateWaitableTimerA(LPSECURITY_ATTRIBUTES security, BOOL manual, LPCSTR name)
{
    return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateWaitableTimerW(security, manual, wide); });
}

HANDLE WINAPI BridgeCreateWaitableTimerExA(LPSECURITY_ATTRIBUTES security, LPCSTR name, DWORD flags, DWORD access)
{
    return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeCreateWaitableTimerExW(security, wide, flags, access); });
}

HANDLE WINAPI BridgeOpenWaitableTimerA(DWORD access, BOOL inherit, LPCSTR name)
{
    return WithAnsiSynchronizationName(name, [&](LPCWSTR wide) { return BridgeOpenWaitableTimerW(access, inherit, wide); });
}

BOOL WINAPI BridgeSetWaitableTimer(HANDLE handle, const LARGE_INTEGER* dueTime, LONG period,
    GuestKernelContext::WaitableTimerApc::Procedure callback, void* parameter, BOOL resume)
{
    return SetGuestWaitableTimer(handle, dueTime, period, callback, parameter, 0, resume != FALSE);
}

BOOL WINAPI BridgeSetWaitableTimerEx(HANDLE handle, const LARGE_INTEGER* dueTime, LONG period,
    GuestKernelContext::WaitableTimerApc::Procedure callback, void* parameter, PREASON_CONTEXT, ULONG tolerableDelay)
{
    return SetGuestWaitableTimer(handle, dueTime, period, callback, parameter, tolerableDelay, false);
}

BOOL WINAPI BridgeCancelWaitableTimer(HANDLE handle)
{
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return FALSE;
    DWORD error = ERROR_SUCCESS;
    if (kernel->CancelTimerObject(handle, &error)) return TRUE;
    SetGuestLastError(error == ERROR_INVALID_FUNCTION ? ERROR_INVALID_HANDLE : error);
    return FALSE;
}

DWORD WINAPI BridgeGetModuleFileNameA(HMODULE module,LPSTR output,DWORD length){
    if(!output||!length){SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);return 0;}
    std::vector<wchar_t> wide(32768);DWORD size=BridgeGetModuleFileNameW(module,wide.data(),static_cast<DWORD>(wide.size()));if(!size)return 0;
    int required=BridgeWideCharToMultiByte(CP_ACP,0,wide.data(),static_cast<int>(size),nullptr,0,nullptr,nullptr);if(required<=0)return 0;
    std::vector<char> bytes(required);if(!BridgeWideCharToMultiByte(CP_ACP,0,wide.data(),size,bytes.data(),required,nullptr,nullptr))return 0;
    DWORD copied=(std::min)(length-1,static_cast<DWORD>(required));if(!CopyConsoleBytes(output,bytes.data(),copied)||!CopyConsoleBytes(output+copied,"",1)){SetGuestLastError(ERROR_NOACCESS);return 0;}
    SetGuestLastError(static_cast<DWORD>(required)>=length?ERROR_INSUFFICIENT_BUFFER:0);return static_cast<DWORD>(required)>=length?length:copied;
}
LCID WINAPI BridgeGetThreadLocale(){return ::GetThreadLocale();}
BOOL WINAPI BridgeSetThreadLocale(LCID locale){BOOL ok=::SetThreadLocale(locale);SetGuestLastError(ok? 0 : ::GetLastError());return ok;}
void WINAPI BridgeGetSystemTime(LPSYSTEMTIME time){if(time)::GetSystemTime(time);}
BOOL WINAPI BridgeSystemTimeToFileTime(const SYSTEMTIME* time,LPFILETIME file){if(!time||!file){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}BOOL ok=::SystemTimeToFileTime(time,file);SetGuestLastError(ok? 0 : ::GetLastError());return ok;}
BOOL WINAPI BridgeSetLocalTime(const SYSTEMTIME*){SetGuestLastError(ERROR_PRIVILEGE_NOT_HELD);return FALSE;} // The guest cannot change the host clock.
void WINAPI BridgeDebugBreak(){BridgeRaiseException(EXCEPTION_BREAKPOINT,0,0,nullptr);}
SIZE_T WINAPI BridgeVirtualQuery(LPCVOID address,PMEMORY_BASIC_INFORMATION result,SIZE_T length){SIZE_T size=::VirtualQuery(address,result,length);SetGuestLastError(size? 0 : ::GetLastError());return size;}
BOOL WINAPI BridgeReadProcessMemory(HANDLE process,LPCVOID source,LPVOID target,SIZE_T count,SIZE_T* copied){
    if(copied)*copied=0;if(process!=BridgeGetCurrentProcess()){SetGuestLastError(ERROR_INVALID_HANDLE);return FALSE;}
    SYSTEM_INFO info{};BridgeGetSystemInfo(&info);const SIZE_T pageSize=info.dwPageSize;
    if(!pageSize){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    const SIZE_T sourceBase=reinterpret_cast<SIZE_T>(source),targetBase=reinterpret_cast<SIZE_T>(target);
    if(count&&((count-1)>(std::numeric_limits<SIZE_T>::max)()-sourceBase||(count-1)>(std::numeric_limits<SIZE_T>::max)()-targetBase)){SetGuestLastError(ERROR_PARTIAL_COPY);return FALSE;}
    SIZE_T done=0;while(done<count){const SIZE_T address=sourceBase+done,destination=targetBase+done;
        const SIZE_T chunk=(std::min)((std::min)(count-done,pageSize-address%pageSize),pageSize-destination%pageSize);
        if(!CopyConsoleBytes(reinterpret_cast<void*>(destination),reinterpret_cast<const void*>(address),chunk)){if(copied)*copied=done;SetGuestLastError(ERROR_PARTIAL_COPY);return FALSE;}done+=chunk;}
    if(copied)*copied=done;SetGuestLastError(0);return TRUE;
}
BOOL WINAPI BridgeGetVolumePathNameW(LPCWSTR path,LPWSTR output,DWORD length){
    auto storage=CurrentGuestStorageContext();std::wstring resolved;DWORD error=0;
    if(!output||length<4){SetGuestLastError(ERROR_FILENAME_EXCED_RANGE);return FALSE;}
    if(!storage||!storage->CanonicalPath(path,&resolved,&error)){SetGuestLastError(storage?error:ERROR_INVALID_HANDLE);return FALSE;}
    std::wstring root=resolved.substr(0,3);if(!CopyConsoleBytes(output,root.c_str(),4*sizeof(wchar_t))){SetGuestLastError(ERROR_NOACCESS);return FALSE;}SetGuestLastError(0);return TRUE;
}
BOOL WINAPI BridgeNeedCurrentDirectoryForExePathW(LPCWSTR name){
    if(!name){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(wcschr(name,L'\\')||wcschr(name,L'/'))return TRUE;
    auto loader=CurrentGuestModuleLoader();std::wstring value;return !(loader&&loader->Environment().Get(L"NoDefaultCurrentDirectoryInExePath",&value));
}
BOOL WINAPI BridgeSetEnvironmentStringsW(LPWCH block){
    if(!block){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}auto loader=CurrentGuestModuleLoader();if(!loader){SetGuestLastError(ERROR_INVALID_FUNCTION);return FALSE;}
    std::vector<std::wstring> entries;
    size_t offset=0;while(offset<32767){wchar_t ch;if(!CopyConsoleBytes(&ch,block+offset,sizeof(ch))){SetGuestLastError(ERROR_NOACCESS);return FALSE;}if(!ch){loader->Environment().Initialize(entries);SetGuestLastError(0);return TRUE;}
        std::wstring entry;do{entry+=ch;if(++offset>=32767){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}if(!CopyConsoleBytes(&ch,block+offset,sizeof(ch))){SetGuestLastError(ERROR_NOACCESS);return FALSE;}}while(ch);++offset;entries.push_back(std::move(entry));}
    SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;
}
DWORD WINAPI BridgeSearchPathW(LPCWSTR search,LPCWSTR file,LPCWSTR extension,DWORD capacity,LPWSTR output,LPWSTR* part){
    if(part)*part=nullptr;if(!file||!*file){SetGuestLastError(ERROR_INVALID_PARAMETER);return 0;}
    std::wstring name=file;if(extension&&*extension&&name.substr(name.find_last_of(L"\\/")+1).find(L'.')==std::wstring::npos)name+=extension;
    auto storage=CurrentGuestStorageContext();auto loader=CurrentGuestModuleLoader();if(!storage){SetGuestLastError(ERROR_INVALID_HANDLE);return 0;}
    std::vector<std::wstring> directories;
    if(wcschr(name.c_str(),L'\\')||wcschr(name.c_str(),L'/')||wcschr(name.c_str(),L':'))directories.push_back(L"");
    else if(search){std::wstring all=search;size_t pos=0;do{auto end=all.find(L';',pos);directories.push_back(all.substr(pos,end-pos));if(end==std::wstring::npos)break;pos=end+1;}while(pos<=all.size());}
    else{const auto module=storage->ModulePath();directories.push_back(module.substr(0,module.find_last_of(L'\\')));directories.push_back(storage->CurrentDirectory());directories.push_back(L"C:\\Windows\\System32");directories.push_back(L"C:\\Windows");if(loader){auto path=loader->Environment().SearchDirectories();directories.insert(directories.end(),path.begin(),path.end());}}
    for(auto directory:directories){if(directory.size()>1&&directory.front()==L'"'&&directory.back()==L'"')directory=directory.substr(1,directory.size()-2);
        std::wstring candidate=directory.empty()?name:directory+L"\\"+name;std::wstring resolved;DWORD error=0;
        if(!storage->CanonicalPath(candidate.c_str(),&resolved,&error))continue;
        DWORD attributes=BridgeGetFileAttributesW(resolved.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY))continue;
        DWORD required=static_cast<DWORD>(resolved.size()+1);if(capacity<required)return required;
        if(!output){SetGuestLastError(ERROR_INVALID_PARAMETER);return 0;}memcpy(output,resolved.c_str(),required*sizeof(wchar_t));if(part)*part=output+resolved.find_last_of(L'\\')+1;SetGuestLastError(0);return required-1;
    }SetGuestLastError(ERROR_FILE_NOT_FOUND);return 0;
}
struct GuestGroupAffinity{ULONG_PTR mask;USHORT group,reserved[3];};
BOOL WINAPI BridgeGetNumaHighestNodeNumber(PULONG highest){if(!highest){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}*highest=0;return TRUE;}
BOOL WINAPI BridgeGetNumaNodeProcessorMaskEx(USHORT node,GuestGroupAffinity* affinity){if(node||!affinity){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}SYSTEM_INFO info{};BridgeGetSystemInfo(&info);*affinity={info.dwActiveProcessorMask,0,{0,0,0}};return TRUE;}
BOOL WINAPI BridgeGetThreadGroupAffinity(HANDLE thread,GuestGroupAffinity* affinity){DWORD id=0,error=0;auto kernel=CurrentGuestKernelContext();if(thread!=reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-2))&&(!kernel||!kernel->GetThreadId(thread,&id,&error))){SetGuestLastError(ERROR_INVALID_HANDLE);return FALSE;}return BridgeGetNumaNodeProcessorMaskEx(0,affinity);}
BOOLEAN WINAPI BridgeCreateSymbolicLinkW(LPCWSTR,LPCWSTR,DWORD){SetGuestLastError(ERROR_NOT_SUPPORTED);return FALSE;}
LONG WINAPI BridgeApiSetQueryApiSetPresence(const GuestUnicodeString* contract,BOOLEAN* present){
    if(!contract||!present)return NtInvalidParameter;*present=FALSE;GuestUnicodeString copy{};
    if(!CopyConsoleBytes(&copy,contract,sizeof(copy))||!copy.buffer||!copy.length||copy.length%2||copy.maximumLength<copy.length)return NtInvalidParameter;
    std::wstring name(copy.length/2,L'\0');if(!CopyConsoleBytes(&name[0],copy.buffer,copy.length))return NtInvalidParameter;
    if(name.find(L'.')==std::wstring::npos)name+=L".dll";
    const std::wstring host=ApiSetHostLibrary(name);*present=FindBridgeSystemModule(host.c_str())?TRUE:FALSE;return 0;
}
PVOID WINAPI BridgeDelayLoadFailureHook(LPCSTR dll,LPCSTR procedure){
    if(!dll||!procedure){SetGuestLastError(ERROR_INVALID_PARAMETER);return nullptr;}
    HMODULE module=BridgeLoadLibraryA(dll);if(!module)return nullptr;
    FARPROC address=BridgeGetProcAddress(module,procedure);if(!address)BridgeFreeLibrary(module);return reinterpret_cast<PVOID>(address);
}
bool PatchGuestDelayPointer(void* slot,void* value){
    DWORD previous=0;if(!BridgeVirtualProtect(slot,sizeof(value),PAGE_READWRITE,&previous))return false;
    const bool copied=CopyConsoleBytes(slot,&value,sizeof(value));DWORD ignored=0;
    const bool restored=BridgeVirtualProtect(slot,sizeof(value),previous,&ignored)!=FALSE;
    return copied&&restored;
}
PVOID WINAPI BridgeResolveDelayLoadedAPI(PVOID parent,PCIMAGE_DELAYLOAD_DESCRIPTOR descriptor,PDELAYLOAD_FAILURE_DLL_CALLBACK dllHook,
    PVOID(WINAPI* systemHook)(LPCSTR,LPCSTR),PIMAGE_THUNK_DATA thunk,ULONG flags){
    if(!parent||!descriptor||!thunk||flags){SetGuestLastError(ERROR_INVALID_PARAMETER);return nullptr;}
    const BYTE* base=nullptr;size_t size=0;auto runtime=CurrentGuestRuntime();auto loader=CurrentGuestModuleLoader();
    if(runtime&&parent==runtime->ImageBase()){base=runtime->ImageBase();size=runtime->ImageSize();}
    else if(!loader||!loader->GetMappedImage(static_cast<HMODULE>(parent),&base,&size)){SetGuestLastError(ERROR_INVALID_HANDLE);return nullptr;}
    auto contains=[&](const void* ptr,size_t length){const auto delta=reinterpret_cast<uintptr_t>(ptr)-reinterpret_cast<uintptr_t>(base);return delta<=size&&length<=size-delta;};
    IMAGE_DELAYLOAD_DESCRIPTOR copy{};
    if(!contains(descriptor,sizeof(copy))||!CopyConsoleBytes(&copy,descriptor,sizeof(copy))||!copy.Attributes.RvaBased){SetGuestLastError(ERROR_INVALID_PARAMETER);return nullptr;}
    auto address=[&](DWORD rva,size_t length)->BYTE*{return rva<=size&&length<=size-rva?const_cast<BYTE*>(base)+rva:nullptr;};
    auto slot=address(copy.ModuleHandleRVA,sizeof(HMODULE));auto iat=address(copy.ImportAddressTableRVA,sizeof(ULONG_PTR));auto names=address(copy.ImportNameTableRVA,sizeof(ULONG_PTR));auto dll=address(copy.DllNameRVA,1);
    if(!slot||!iat||!names||!dll||!contains(thunk,sizeof(*thunk))||reinterpret_cast<BYTE*>(thunk)<iat){SetGuestLastError(ERROR_INVALID_PARAMETER);return nullptr;}
    const size_t delta=reinterpret_cast<BYTE*>(thunk)-iat;if(delta%sizeof(ULONG_PTR)||!contains(names+delta,sizeof(ULONG_PTR))){SetGuestLastError(ERROR_INVALID_PARAMETER);return nullptr;}
    const char* library=reinterpret_cast<const char*>(dll);if(!memchr(library,0,size-(dll-base))){SetGuestLastError(ERROR_INVALID_NAME);return nullptr;}
    ULONG_PTR entry=0;CopyConsoleBytes(&entry,names+delta,sizeof(entry));const char* proc;
    if(entry&IMAGE_ORDINAL_FLAG64)proc=reinterpret_cast<const char*>(entry&65535);
    else {if(entry>MAXDWORD){SetGuestLastError(ERROR_INVALID_PARAMETER);return nullptr;}auto name=address(static_cast<DWORD>(entry),3);if(!name||!memchr(name+2,0,size-(name+2-base))){SetGuestLastError(ERROR_INVALID_NAME);return nullptr;}proc=reinterpret_cast<const char*>(name+2);}
    HMODULE module=nullptr;CopyConsoleBytes(&module,slot,sizeof(module));
    if(!module){module=BridgeLoadLibraryA(library);if(module&&!PatchGuestDelayPointer(slot,module)){BridgeFreeLibrary(module);return nullptr;}}
    void* target=module?reinterpret_cast<void*>(BridgeGetProcAddress(module,proc)):nullptr;
    if(target){if(!PatchGuestDelayPointer(thunk,target))return nullptr;return target;}
    DELAYLOAD_INFO failure{};failure.Size=sizeof(failure);failure.DelayloadDescriptor=descriptor;failure.ThunkAddress=thunk;failure.TargetDllName=library;
    failure.TargetApiDescriptor.ImportDescribedByName=(entry&IMAGE_ORDINAL_FLAG64)==0;
    if(failure.TargetApiDescriptor.ImportDescribedByName)failure.TargetApiDescriptor.Description.Name=proc;else failure.TargetApiDescriptor.Description.Ordinal=static_cast<ULONG>(entry&65535);
    failure.TargetModuleBase=module;failure.LastError=static_cast<ULONG>(NativeStatus(BridgeGetLastError()));
    if(dllHook){void* result=nullptr;if(InvokeCrtFunction<void*>(&result,dllHook,DELAYLOAD_GPA_FAILURE,&failure))return result;return nullptr;}
    if(systemHook){void* result=nullptr;if(InvokeCrtFunction<void*>(&result,systemHook,library,proc))return result;}
    return nullptr;
}
struct GuestTimerInvocation {PTP_TIMER_CALLBACK callback;void* context;PTP_TIMER timer;};
DWORD WINAPI InvokeGuestTimerCallback(void* value){auto invocation=static_cast<GuestTimerInvocation*>(value);invocation->callback(nullptr,invocation->context,invocation->timer);return 0;}
PTP_TIMER WINAPI BridgeCreateThreadpoolTimer(PTP_TIMER_CALLBACK callback,PVOID context,PTP_CALLBACK_ENVIRON environment){
    auto kernel=CurrentGuestKernelContext();auto runtime=CurrentGuestRuntime();
    if(!callback||!kernel||!runtime||environment){SetGuestLastError(environment?ERROR_NOT_SUPPORTED:ERROR_INVALID_PARAMETER);return nullptr;}
    try{auto handle=kernel->Timers().Create(callback,context,[kernel,runtime](PTP_TIMER_CALLBACK function,void* argument,PTP_TIMER timer){
        GuestTimerInvocation invocation{function,argument,timer};DWORD error=0;
        HANDLE thread=runtime->LaunchThread(0,&InvokeGuestTimerCallback,&invocation,0,nullptr,&error);
        if(thread){kernel->WaitForSingleObject(thread,INFINITE,&error);kernel->CloseHandle(thread,&error);}
    });SetGuestLastError(handle?0:ERROR_INVALID_HANDLE);return handle;}catch(...){SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
}
void WINAPI BridgeSetThreadpoolTimer(PTP_TIMER timer,PFILETIME due,DWORD period,DWORD){auto kernel=CurrentGuestKernelContext();FILETIME copy{};if(due&&!CopyConsoleBytes(&copy,due,sizeof(copy))){SetGuestLastError(ERROR_NOACCESS);return;}SetGuestLastError(kernel&&kernel->Timers().Set(timer,due?&copy:nullptr,period)?0:ERROR_INVALID_HANDLE);}
void WINAPI BridgeWaitForThreadpoolTimerCallbacks(PTP_TIMER timer,BOOL cancel){auto kernel=CurrentGuestKernelContext();SetGuestLastError(kernel&&kernel->Timers().Wait(timer,cancel!=FALSE)?0:ERROR_INVALID_HANDLE);}
void WINAPI BridgeCloseThreadpoolTimer(PTP_TIMER timer){auto kernel=CurrentGuestKernelContext();SetGuestLastError(kernel&&kernel->Timers().Close(timer)?0:ERROR_INVALID_HANDLE);}
HANDLE WINAPI BridgeOpenThread(DWORD, BOOL inherit, DWORD id)
{
    auto kernel = CurrentGuestKernelContext();
    DWORD error = ERROR_INVALID_HANDLE;
    const HANDLE handle = kernel ? kernel->OpenCompletion(id, false, inherit != FALSE, &error) : nullptr;
    SetGuestLastError(error);
    return handle;
}
BOOL WINAPI BridgeCreateProcessAsUserW(HANDLE token,LPCWSTR app,LPWSTR command,LPSECURITY_ATTRIBUTES process,LPSECURITY_ATTRIBUTES thread,BOOL inherit,DWORD flags,void* environment,LPCWSTR directory,LPSTARTUPINFOW startup,LPPROCESS_INFORMATION information){
    // Only the guest's existing identity is available. Never impersonate a host user.
    if(token){DWORD size=0;BridgeGetTokenInformation(token,TokenUser,nullptr,0,&size);if(!size){SetGuestLastError(ERROR_INVALID_HANDLE);return FALSE;}}
    return BridgeCreateProcessW(app,command,process,thread,inherit,flags,environment,directory,startup,information);
}
struct GuestAttributeListHeader{ULONGLONG magic;DWORD capacity,count;};
BOOL WINAPI BridgeInitializeProcThreadAttributeList(void* list,DWORD count,DWORD flags,SIZE_T* size){
    if(!size||flags||count>64){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    SIZE_T required=sizeof(GuestAttributeListHeader)+static_cast<SIZE_T>(count)*3*sizeof(ULONG_PTR);SIZE_T supplied=*size;*size=required;
    if(!list||supplied<required){SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    GuestAttributeListHeader header{0x5742333241545452ull,count,0};if(!CopyConsoleBytes(list,&header,sizeof(header))){SetGuestLastError(ERROR_NOACCESS);return FALSE;}SetGuestLastError(0);return TRUE;
}
BOOL WINAPI BridgeUpdateProcThreadAttribute(void* list,DWORD,ULONG_PTR,void*,SIZE_T,void*,SIZE_T*){
    GuestAttributeListHeader header{};if(!list||!CopyConsoleBytes(&header,list,sizeof(header))||header.magic!=0x5742333241545452ull){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    // CreateProcess currently rejects EXTENDED_STARTUPINFO_PRESENT. Do not
    // accept attributes that would be silently ignored at process creation.
    SetGuestLastError(ERROR_NOT_SUPPORTED);return FALSE;
}
void WINAPI BridgeDeleteProcThreadAttributeList(void* list){GuestAttributeListHeader empty{};if(list)CopyConsoleBytes(list,&empty,sizeof(empty));}
ImportResolution ResolveExtendedKernelImport(const ImportedSymbol& symbol){
    ImportResolution result{ImportDisposition::Unsupported,L"No extended kernel adapter.",0};
#define KERNEL_EXTRA(exportName,function) if(symbol.name==L##exportName)result.targetAddress=reinterpret_cast<ULONGLONG>(&function);else
    KERNEL_EXTRA("ApiSetQueryApiSetPresence",BridgeApiSetQueryApiSetPresence)
    KERNEL_EXTRA("ResolveDelayLoadedAPI",BridgeResolveDelayLoadedAPI)
    KERNEL_EXTRA("DelayLoadFailureHook",BridgeDelayLoadFailureHook)
    KERNEL_EXTRA("CreateThreadpoolTimer",BridgeCreateThreadpoolTimer)
    KERNEL_EXTRA("SetThreadpoolTimer",BridgeSetThreadpoolTimer)
    KERNEL_EXTRA("WaitForThreadpoolTimerCallbacks",BridgeWaitForThreadpoolTimerCallbacks)
    KERNEL_EXTRA("CloseThreadpoolTimer",BridgeCloseThreadpoolTimer)
    KERNEL_EXTRA("OpenThread",BridgeOpenThread)
    KERNEL_EXTRA("CreateProcessAsUserW",BridgeCreateProcessAsUserW)
    KERNEL_EXTRA("InitializeProcThreadAttributeList",BridgeInitializeProcThreadAttributeList)
    KERNEL_EXTRA("UpdateProcThreadAttribute",BridgeUpdateProcThreadAttribute)
    KERNEL_EXTRA("DeleteProcThreadAttributeList",BridgeDeleteProcThreadAttributeList)
    KERNEL_EXTRA("AcquireSRWLockShared",BridgeAcquireSRWLockShared)
    KERNEL_EXTRA("ReleaseSRWLockShared",BridgeReleaseSRWLockShared)
    KERNEL_EXTRA("TryAcquireSRWLockShared",BridgeTryAcquireSRWLockShared)
    KERNEL_EXTRA("InitializeSRWLock",BridgeInitializeSRWLock)
    KERNEL_EXTRA("InitializeConditionVariable",BridgeInitializeConditionVariable)
    KERNEL_EXTRA("SleepConditionVariableCS",BridgeSleepConditionVariableCS)
    KERNEL_EXTRA("WakeConditionVariable",BridgeWakeConditionVariable)
    KERNEL_EXTRA("WaitOnAddress",BridgeWaitOnAddress)
    KERNEL_EXTRA("WakeByAddressSingle",BridgeWakeByAddressSingle)
    KERNEL_EXTRA("WakeByAddressAll",BridgeWakeByAddressAll)
    KERNEL_EXTRA("TryEnterCriticalSection",BridgeTryEnterCriticalSection)
    KERNEL_EXTRA("SetCriticalSectionSpinCount",BridgeSetCriticalSectionSpinCount)
    KERNEL_EXTRA("InitOnceInitialize",BridgeInitOnceInitialize)
    KERNEL_EXTRA("InitOnceExecuteOnce",BridgeInitOnceExecuteOnce)
    KERNEL_EXTRA("CreateMutexExW",BridgeCreateMutexExW)
    KERNEL_EXTRA("CreateEventA",BridgeCreateEventA)
    KERNEL_EXTRA("CreateEventExA",BridgeCreateEventExA)
    KERNEL_EXTRA("CreateEventExW",BridgeCreateEventExW)
    KERNEL_EXTRA("OpenEventA",BridgeOpenEventA)
    KERNEL_EXTRA("CreateMutexA",BridgeCreateMutexA)
    KERNEL_EXTRA("CreateMutexExA",BridgeCreateMutexExA)
    KERNEL_EXTRA("OpenMutexA",BridgeOpenMutexA)
    KERNEL_EXTRA("OpenMutexW",BridgeOpenMutexW)
    KERNEL_EXTRA("CreateSemaphoreA",BridgeCreateSemaphoreA)
    KERNEL_EXTRA("CreateSemaphoreExA",BridgeCreateSemaphoreExA)
    KERNEL_EXTRA("OpenSemaphoreA",BridgeOpenSemaphoreA)
    KERNEL_EXTRA("CreateSemaphoreExW",BridgeCreateSemaphoreExW)
    KERNEL_EXTRA("OpenSemaphoreW",BridgeOpenSemaphoreW)
    KERNEL_EXTRA("CreateWaitableTimerA",BridgeCreateWaitableTimerA)
    KERNEL_EXTRA("CreateWaitableTimerW",BridgeCreateWaitableTimerW)
    KERNEL_EXTRA("CreateWaitableTimerExA",BridgeCreateWaitableTimerExA)
    KERNEL_EXTRA("CreateWaitableTimerExW",BridgeCreateWaitableTimerExW)
    KERNEL_EXTRA("OpenWaitableTimerA",BridgeOpenWaitableTimerA)
    KERNEL_EXTRA("OpenWaitableTimerW",BridgeOpenWaitableTimerW)
    KERNEL_EXTRA("SetWaitableTimer",BridgeSetWaitableTimer)
    KERNEL_EXTRA("SetWaitableTimerEx",BridgeSetWaitableTimerEx)
    KERNEL_EXTRA("CancelWaitableTimer",BridgeCancelWaitableTimer)
    KERNEL_EXTRA("CreateIoCompletionPort",BridgeCreateIoCompletionPort)
    KERNEL_EXTRA("PostQueuedCompletionStatus",BridgePostQueuedCompletionStatus)
    KERNEL_EXTRA("GetQueuedCompletionStatus",BridgeGetQueuedCompletionStatus)
    KERNEL_EXTRA("GetQueuedCompletionStatusEx",BridgeGetQueuedCompletionStatusEx)
    KERNEL_EXTRA("SetFileCompletionNotificationModes",BridgeSetFileCompletionNotificationModes)
    KERNEL_EXTRA("CancelIoEx",BridgeCancelIoEx)
    KERNEL_EXTRA("ReadFileEx",BridgeReadFileEx)
    KERNEL_EXTRA("WriteFileEx",BridgeWriteFileEx)
    KERNEL_EXTRA("GetModuleFileNameA",BridgeGetModuleFileNameA)
    KERNEL_EXTRA("GetThreadLocale",BridgeGetThreadLocale)
    KERNEL_EXTRA("SetThreadLocale",BridgeSetThreadLocale)
    KERNEL_EXTRA("GetSystemTime",BridgeGetSystemTime)
    KERNEL_EXTRA("SystemTimeToFileTime",BridgeSystemTimeToFileTime)
    KERNEL_EXTRA("SetLocalTime",BridgeSetLocalTime)
    KERNEL_EXTRA("DebugBreak",BridgeDebugBreak)
    KERNEL_EXTRA("VirtualQuery",BridgeVirtualQuery)
    KERNEL_EXTRA("ReadProcessMemory",BridgeReadProcessMemory)
    KERNEL_EXTRA("GetVolumePathNameW",BridgeGetVolumePathNameW)
    KERNEL_EXTRA("NeedCurrentDirectoryForExePathW",BridgeNeedCurrentDirectoryForExePathW)
    KERNEL_EXTRA("SetEnvironmentStringsW",BridgeSetEnvironmentStringsW)
    KERNEL_EXTRA("SearchPathW",BridgeSearchPathW)
    KERNEL_EXTRA("GetNumaHighestNodeNumber",BridgeGetNumaHighestNodeNumber)
    KERNEL_EXTRA("GetNumaNodeProcessorMaskEx",BridgeGetNumaNodeProcessorMaskEx)
    KERNEL_EXTRA("GetThreadGroupAffinity",BridgeGetThreadGroupAffinity)
    KERNEL_EXTRA("CreateSymbolicLinkW",BridgeCreateSymbolicLinkW)
    {}
#undef KERNEL_EXTRA
    if(result.targetAddress){result.disposition=ImportDisposition::NeedsBridge;result.note=L"Guest kernel adapter; privileged/unsupported operations report explicit Win32 failure.";}return result;
}
} }
