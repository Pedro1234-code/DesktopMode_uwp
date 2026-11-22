// Optional API contracts: generic Windows exports, never executable-name checks.
namespace {
using namespace Win32Bridge::Bridge;
LPWSTR WINAPI GuestBrandingFormatString(LPCWSTR format){
    if(!format){BridgeSetLastError(ERROR_INVALID_PARAMETER);return nullptr;}
    std::wstring product=L"Windows";HKEY key=nullptr;
    if(BridgeRegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_QUERY_VALUE,&key)==0){
        wchar_t value[256]{};DWORD bytes=sizeof(value),type=0;
        if(BridgeRegQueryValueExW(key,L"ProductName",nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes)==0&&type==REG_SZ&&bytes>=sizeof(wchar_t)&&bytes<=sizeof(value)){value[255]=0;product=value;}
        BridgeRegCloseKey(key);
    }
    std::wstring result=format;
    for(const auto token:{L"%WINDOWS_LONG%",L"%WINDOWS_SHORT%"}){size_t pos=0;while((pos=result.find(token,pos))!=std::wstring::npos){result.replace(pos,wcslen(token),product);pos+=product.size();}}
    auto output=static_cast<wchar_t*>(BridgeHeapAlloc(BridgeGetProcessHeap(),0,(result.size()+1)*sizeof(wchar_t)));if(!output)return nullptr;
    memcpy(output,result.c_str(),(result.size()+1)*sizeof(wchar_t));return output;
}
BOOL WINAPI GuestLookupAccountSidW(LPCWSTR system,PSID sid,LPWSTR name,LPDWORD nameLength,LPWSTR domain,LPDWORD domainLength,PSID_NAME_USE use){
    if(!sid||!nameLength||!domainLength||!use){BridgeSetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(system&&*system&&wcscmp(system,L".")&&_wcsicmp(system,L"Win32Bridge")){BridgeSetLastError(ERROR_NONE_MAPPED);return FALSE;}
    const BYTE* data=static_cast<const BYTE*>(sid);
    // Public SID layout; only the identities present in the virtual namespace map.
    if(data[0]!=1||data[1]>15){BridgeSetLastError(ERROR_INVALID_SID);return FALSE;}
    std::wstring user,group;SID_NAME_USE kind=SidTypeUnknown;
    const BYTE virtualUser[]={1,5,0,0,0,0,0,5,21,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xe8,3,0,0};
    const BYTE world[]={1,1,0,0,0,0,0,1,0,0,0,0};const BYTE authenticated[]={1,1,0,0,0,0,0,5,11,0,0,0};
    const size_t bytes=8+static_cast<size_t>(data[1])*4;
    if(bytes==sizeof(virtualUser)&&!memcmp(data,virtualUser,bytes)){user=L"Win32Bridge";group=L"Win32Bridge";kind=SidTypeUser;}
    else if(bytes==sizeof(world)&&!memcmp(data,world,bytes)){user=L"Everyone";kind=SidTypeWellKnownGroup;}
    else if(bytes==sizeof(authenticated)&&!memcmp(data,authenticated,bytes)){user=L"Authenticated Users";group=L"NT AUTHORITY";kind=SidTypeWellKnownGroup;}
    else{BridgeSetLastError(ERROR_NONE_MAPPED);return FALSE;}
    const DWORD requiredName=static_cast<DWORD>(user.size()+1),requiredDomain=static_cast<DWORD>(group.size()+1);
    if(!name||!domain||*nameLength<requiredName||*domainLength<requiredDomain){*nameLength=requiredName;*domainLength=requiredDomain;BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    memcpy(name,user.c_str(),requiredName*sizeof(wchar_t));memcpy(domain,group.c_str(),requiredDomain*sizeof(wchar_t));*nameLength=requiredName-1;*domainLength=requiredDomain-1;if(use)*use=kind;BridgeSetLastError(0);return TRUE;
}
DWORD WINAPI GuestWNetCancelConnection2W(LPCWSTR,DWORD,BOOL){return ERROR_NOT_CONNECTED;}
DWORD WINAPI GuestWNetGetConnectionW(LPCWSTR,LPWSTR,LPDWORD length){return length?ERROR_NOT_CONNECTED:ERROR_INVALID_PARAMETER;}
BOOL WINAPI GuestMessageBeep(UINT){BridgeSetLastError(ERROR_NOT_SUPPORTED);return FALSE;}
DWORD WINAPI GuestGetVDMCurrentDirectories(DWORD,LPSTR){BridgeSetLastError(ERROR_NOT_SUPPORTED);return 0;}
void WINAPI GuestCmdBatNotification(DWORD state){RuntimeDiagnostics::Record(L"CONSOLE BATCH: state="+std::to_wstring(state));}
DWORD WINAPI GuestSaferWorker(const void*,HANDLE* level,HANDLE* token,LPCWSTR,BOOL* blocked){
    if(level)*level=nullptr;if(token)*token=nullptr;if(blocked)*blocked=FALSE;
    BridgeSetLastError(ERROR_NOT_SUPPORTED);return 1; // Failure, never a simulated policy approval.
}
struct GuestShellExecuteInfo {DWORD size,mask;HWND owner;LPCWSTR verb,file,parameters,directory;INT show;HINSTANCE instance;void* idList;LPCWSTR className;HKEY classKey;DWORD hotKey;HANDLE iconOrMonitor,process;};
static_assert(sizeof(GuestShellExecuteInfo)==112,"SHELLEXECUTEINFOW x64 ABI");
BOOL WINAPI GuestShellExecuteWorker(DWORD flags,LPCWSTR file,LPCWSTR arguments,LPCWSTR directory,INT show,HANDLE* process,DWORD* error){
    if(process)*process=nullptr;GuestShellExecuteInfo info{};info.size=sizeof(info);info.mask=(flags&0x10)?0x140:0x8140;
    info.file=file;info.parameters=arguments;info.directory=directory;info.show=show;
    const BOOL ok=BridgeShellExecuteExW(&info);
    if(ok){if(process)*process=info.process;if(error)*error=0;}
    else if(error){const auto value=reinterpret_cast<ULONG_PTR>(info.instance);*error=!value?8:value==32?2:static_cast<DWORD>(value);}
    return ok;
}
HRESULT WINAPI GuestWldpCanExecuteFile(const GUID*,ULONG,HANDLE,LPCWSTR,ULONG* policy){if(!policy)return E_POINTER;*policy=0;return E_NOTIMPL;}
HRESULT WINAPI GuestWldpGetApplicationSettingBoolean(LPCWSTR,LPCWSTR,BOOL* result){if(!result)return E_POINTER;*result=FALSE;return E_NOTIMPL;}
ImportResolution ResolveOptionalContractImport(const ImportedSymbol& symbol){
    ImportResolution result{ImportDisposition::Unsupported,L"No optional contract implementation.",0};
    auto assign=[&](ULONGLONG address){result.targetAddress=address;result.disposition=ImportDisposition::NeedsBridge;result.note=L"Guest optional-contract adapter; unavailable services return explicit failure.";};
    if(_wcsicmp(symbol.library.c_str(),L"wldp.dll")==0){
        if((symbol.importedByOrdinal&&symbol.ordinal==2)||(!symbol.importedByOrdinal&&symbol.name==L"WldpCanExecuteFile"))assign(reinterpret_cast<ULONGLONG>(&GuestWldpCanExecuteFile));
        else if((symbol.importedByOrdinal&&symbol.ordinal==10)||(!symbol.importedByOrdinal&&symbol.name==L"WldpGetApplicationSettingBoolean"))assign(reinterpret_cast<ULONGLONG>(&GuestWldpGetApplicationSettingBoolean));
        return result;
    }
    if(symbol.importedByOrdinal)return result;
    if(_wcsicmp(symbol.library.c_str(),L"winbrand.dll")==0&&symbol.name==L"BrandingFormatString"){assign(reinterpret_cast<ULONGLONG>(&GuestBrandingFormatString));return result;}
    if(_wcsicmp(symbol.library.c_str(),L"advapi32.dll")==0&&symbol.name==L"LookupAccountSidW"){assign(reinterpret_cast<ULONGLONG>(&GuestLookupAccountSidW));return result;}
    if(_wcsicmp(symbol.library.c_str(),L"cmdext.dll"))return result;
    ImportedSymbol target=symbol;target.library=L"kernel32.dll";
    if(symbol.name==L"FindFirstStreamWStub")target.name=L"FindFirstStreamW";
    else if(symbol.name==L"FindNextStreamWStub")target.name=L"FindNextStreamW";
    else if(symbol.name==L"QueryFullProcessImageNameWStub")target.name=L"QueryFullProcessImageNameW";
    else if(symbol.name==L"LookupAccountSidWStub"){target.library=L"advapi32.dll";target.name=L"LookupAccountSidW";}
    else if(symbol.name==L"WNetAddConnection2WStub"){target.library=L"mpr.dll";target.name=L"WNetAddConnection2W";}
    else if(symbol.name==L"DoSHChangeNotify"){assign(reinterpret_cast<ULONGLONG>(&BridgeSHChangeNotify));return result;}
    else if(symbol.name==L"WNetCancelConnection2WStub"){assign(reinterpret_cast<ULONGLONG>(&GuestWNetCancelConnection2W));return result;}
    else if(symbol.name==L"WNetGetConnectionWStub"){assign(reinterpret_cast<ULONGLONG>(&GuestWNetGetConnectionW));return result;}
    else if(symbol.name==L"MessageBeepStub"){assign(reinterpret_cast<ULONGLONG>(&GuestMessageBeep));return result;}
    else if(symbol.name==L"GetVDMCurrentDirectoriesStub"){assign(reinterpret_cast<ULONGLONG>(&GuestGetVDMCurrentDirectories));return result;}
    else if(symbol.name==L"CmdBatNotificationStub"){assign(reinterpret_cast<ULONGLONG>(&GuestCmdBatNotification));return result;}
    else if(symbol.name==L"ShellExecuteWorker"){assign(reinterpret_cast<ULONGLONG>(&GuestShellExecuteWorker));return result;}
    else if(symbol.name==L"SaferWorker"){assign(reinterpret_cast<ULONGLONG>(&GuestSaferWorker));return result;}
    else return result;
    return ResolveRuntimeImport(target);
}
}
