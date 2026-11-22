namespace {
int __cdecl CrtGetch() {
    using namespace Win32Bridge::Bridge;
    HANDLE input=BridgeGetStdHandle(static_cast<DWORD>(-10));DWORD previous=0;
    if(!BridgeGetConsoleMode(input,&previous))return CrtIoFailure(BridgeGetLastError());
    if(!BridgeSetConsoleMode(input,previous&~6u))return CrtIoFailure(BridgeGetLastError());
    unsigned char ch=0;DWORD read=0;
    const bool ok=BridgeReadConsoleA(input,&ch,1,&read,nullptr)!=FALSE;const DWORD error=BridgeGetLastError();
    BridgeSetConsoleMode(input,previous);
    return ok?(read?ch:EOF):CrtIoFailure(error);
}
wchar_t* __cdecl CrtLower(wchar_t* text){if(!text){errno=EINVAL;return nullptr;}return ::_wcslwr_s_l(text,wcslen(text)+1,Win32Bridge::Bridge::CurrentCrtLocaleHandle())?nullptr:text;}
wchar_t* __cdecl CrtUpper(wchar_t* text){if(!text){errno=EINVAL;return nullptr;}return ::_wcsupr_s_l(text,wcslen(text)+1,Win32Bridge::Bridge::CurrentCrtLocaleHandle())?nullptr:text;}
char* __cdecl CrtUltoa(unsigned long value,char* text,int radix){if(!text){errno=EINVAL;return nullptr;}return ::_ultoa_s(value,text,33,radix)?nullptr:text;}
int __cdecl CrtSehFilter(unsigned long code,void*) {
    // Equivalent to the CRT filter's search disposition for faults without a
    // registered guest signal handler. Never consume a native access violation.
    (void)code;return EXCEPTION_CONTINUE_SEARCH;
}
void* __cdecl CrtWPopen(const wchar_t* command,const wchar_t* mode) {
    using namespace Win32Bridge::Bridge;
    auto runtime=CurrentGuestRuntime();auto loader=CurrentGuestModuleLoader();auto state=CurrentCrt();
    if(!runtime||!loader||!state||!command||!mode||(*mode!=L'r'&&*mode!=L'w')){errno=EINVAL;return nullptr;}
    bool reading=*mode==L'r',binary=false;
    if(mode[1]){if((mode[1]!=L'b'&&mode[1]!=L't')||mode[2]){errno=EINVAL;return nullptr;}binary=mode[1]==L'b';}
    SECURITY_ATTRIBUTES sa{};sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
    HANDLE reader=nullptr,writer=nullptr;
    if(!BridgeCreatePipe(&reader,&writer,&sa,4096)){CrtIoFailure(BridgeGetLastError());return nullptr;}
    HANDLE parent=reading?reader:writer,child=reading?writer:reader;
    if(!BridgeSetHandleInformation(parent,1,0)){BridgeCloseHandle(reader);BridgeCloseHandle(writer);CrtIoFailure(BridgeGetLastError());return nullptr;}
    std::wstring shell;
    if(!loader->Environment().Get(L"COMSPEC",&shell)||shell.empty())shell=L"C:\\Windows\\System32\\cmd.exe";
    std::wstring line=L"\""+shell+L"\" /d /s /c \""+command+L"\"";
    HANDLE standards[3];for(unsigned i=0;i<3;++i)standards[i]=BridgeGetStdHandle(static_cast<DWORD>(-10-static_cast<int>(i)));
    standards[reading?1:0]=child;
    PROCESS_INFORMATION process{};DWORD error=0;
    if(!runtime->LaunchChildProcess(shell.c_str(),line.c_str(),nullptr,&process,&error,nullptr,0,true,standards)){
        BridgeCloseHandle(reader);BridgeCloseHandle(writer);CrtIoFailure(error);return nullptr;
    }
    BridgeCloseHandle(child);BridgeCloseHandle(process.hThread);
    const int fd=loader->CrtFiles().Adopt(parent,(reading?_O_RDONLY:_O_WRONLY)|(binary?_O_BINARY:_O_TEXT)|_O_NOINHERIT,true);
    if(fd<0){BridgeCloseHandle(process.hProcess);return nullptr;}
    void* stream=loader->Stdio().Attach(fd,mode,false);
    if(!stream){loader->CrtFiles().Close(fd);BridgeCloseHandle(process.hProcess);return nullptr;}
    try {std::lock_guard<std::recursive_mutex> guard(state->ExitMutex());state->pipeProcesses.emplace(stream,process.hProcess);}
    catch(const std::bad_alloc&){loader->Stdio().Close(stream);BridgeCloseHandle(process.hProcess);errno=ENOMEM;return nullptr;}
    return stream;
}
int __cdecl CrtPClose(void* stream) {
    using namespace Win32Bridge::Bridge;
    auto state=CurrentCrt();auto loader=CurrentGuestModuleLoader();if(!state||!loader){errno=EINVAL;return -1;}
    HANDLE process;
    {std::lock_guard<std::recursive_mutex> guard(state->ExitMutex());auto found=state->pipeProcesses.find(stream);if(found==state->pipeProcesses.end()){errno=EBADF;return -1;}process=found->second;state->pipeProcesses.erase(found);}
    const int closed=loader->Stdio().Close(stream);const DWORD waited=BridgeWaitForSingleObject(process,INFINITE);DWORD result=0;
    const bool ok=waited==WAIT_OBJECT_0&&BridgeGetExitCodeProcess(process,&result);const DWORD error=BridgeGetLastError();BridgeCloseHandle(process);
    if(!ok)return CrtIoFailure(error);return closed<0?-1:static_cast<int>(result);
}
Win32Bridge::Bridge::ImportResolution ResolveCrtConsoleProcess(const Win32Bridge::Bridge::ImportedSymbol& symbol){
    using namespace Win32Bridge::Bridge;
    ImportResolution result{ImportDisposition::Unsupported,L"No extra CRT adapter.",0};
#define CRT_EXTRA(exportName,function) if(symbol.name==L##exportName)result.targetAddress=reinterpret_cast<ULONGLONG>(&function);else
    CRT_EXTRA("_getch",CrtGetch)
    CRT_EXTRA("_wcslwr",CrtLower)
    CRT_EXTRA("_wcsupr",CrtUpper)
    CRT_EXTRA("_ultoa",CrtUltoa)
    CRT_EXTRA("_seh_filter_exe",CrtSehFilter)
    CRT_EXTRA("_wpopen",CrtWPopen)
    CRT_EXTRA("_pclose",CrtPClose)
    {}
#undef CRT_EXTRA
    if(result.targetAddress){result.disposition=ImportDisposition::NeedsBridge;result.note=L"Guest console/process CRT adapter.";}return result;
}
}
