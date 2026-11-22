"""Native Win32 file-description reference contracts; no bridge C++ build."""
import ctypes as c
from ctypes import wintypes as w
import tempfile, pathlib, subprocess, sys
k=c.WinDLL('kernel32',use_last_error=True)
class Security(c.Structure):
    _fields_=[('length',w.DWORD),('descriptor',c.c_void_p),('inherit',w.BOOL)]
k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.POINTER(Security),w.DWORD,w.DWORD,w.HANDLE];k.CreateFileW.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
k.DuplicateHandle.argtypes=[w.HANDLE,w.HANDLE,w.HANDLE,c.POINTER(w.HANDLE),w.DWORD,w.BOOL,w.DWORD];k.DuplicateHandle.restype=w.BOOL
k.GetHandleInformation.argtypes=[w.HANDLE,c.POINTER(w.DWORD)];k.GetHandleInformation.restype=w.BOOL
k.SetHandleInformation.argtypes=[w.HANDLE,w.DWORD,w.DWORD];k.SetHandleInformation.restype=w.BOOL
k.SetFilePointerEx.argtypes=[w.HANDLE,c.c_longlong,c.POINTER(c.c_longlong),w.DWORD];k.SetFilePointerEx.restype=w.BOOL
k.ReadFile.argtypes=[w.HANDLE,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.c_void_p];k.ReadFile.restype=w.BOOL
k.WriteFile.argtypes=k.ReadFile.argtypes;k.WriteFile.restype=w.BOOL
k.GetFileAttributesW.argtypes=[w.LPCWSTR];k.GetFileAttributesW.restype=w.DWORD
passed=0; handles=[]; invalid=w.HANDLE(-1).value; process=w.HANDLE(-1)
def check(ok,label):
    global passed
    if not ok: raise AssertionError(label)
    passed+=1

def open_file(path,share=7,flags=0,inherit=False):
    security=Security(c.sizeof(Security),None,inherit)
    h=k.CreateFileW(str(path),0xc0000000,share,c.byref(security),2,flags,None)
    if h!=invalid:handles.append(h)
    return h

def close(h):
    ok=k.CloseHandle(h)
    if ok:handles.remove(h)
    return ok

with tempfile.TemporaryDirectory(prefix='bridge-file-contract-') as folder:
    try:
        path=pathlib.Path(folder)/'shared.txt'; original=open_file(path,share=0,inherit=True); flags=w.DWORD();n=w.DWORD();pos=c.c_longlong()
        check(original!=invalid,'Create inherited file')
        check(k.GetHandleInformation(original,c.byref(flags)) and flags.value==1,'CreateFile security attributes set inherit flag')
        check(k.WriteFile(original,c.create_string_buffer(b'ab'),2,c.byref(n),None) and n.value==2,'Write initial prefix')
        duplicate=w.HANDLE();check(k.DuplicateHandle(process,original,process,c.byref(duplicate),0,False,2),'Duplicate same access');handles.append(duplicate.value)
        check(k.GetHandleInformation(duplicate,c.byref(flags)) and flags.value==0,'Duplicate inheritance independent of source')
        check(k.SetFilePointerEx(duplicate,0,c.byref(pos),1) and pos.value==2,'Duplicate starts at shared current position')
        check(k.WriteFile(duplicate,c.create_string_buffer(b'cd'),2,c.byref(n),None),'Write through duplicate')
        check(k.SetFilePointerEx(original,0,c.byref(pos),1) and pos.value==4,'Original observes duplicate position advance')
        check(close(original),'Close original')
        check(k.WriteFile(duplicate,c.create_string_buffer(b'ef'),2,c.byref(n),None),'Duplicate survives source close')
        peer=k.CreateFileW(str(path),0x80000000,7,None,3,0,None)
        if peer!=invalid:handles.append(peer)
        check(peer==invalid and c.get_last_error()==32,'Share denial survives original close')
        check(close(duplicate.value),'Close last duplicate')
        check(path.read_bytes()==b'abcdef','Shared writes form one stream')
        # Use a headless native Python child to exercise actual OS inheritance;
        # this does not launch a guest through Win32Bridge.
        inherited=open_file(path,inherit=True)
        check(inherited!=invalid,'Create child stdout file')
        check(k.WriteFile(inherited,c.create_string_buffer(b'parent:'),7,c.byref(n),None),'Parent prefix before child')
        startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESTDHANDLES
        startup.hStdInput=None;startup.hStdOutput=inherited;startup.hStdError=inherited
        startup.lpAttributeList={'handle_list':[inherited]}
        child=subprocess.Popen([sys.executable,'-c',"import os; os.write(1,b'child')"],startupinfo=startup,close_fds=True,creationflags=0x08000000)
        check(child.wait(timeout=15)==0,'Native child writes inherited stdout')
        check(k.SetFilePointerEx(inherited,0,c.byref(pos),1) and pos.value==12,'Parent sees child position advance')
        check(k.WriteFile(inherited,c.create_string_buffer(b':tail'),5,c.byref(n),None),'Parent file still usable after child exit')
        check(close(inherited) and path.read_bytes()==b'parent:child:tail','Child exit does not close parent description')
        deleted=pathlib.Path(folder)/'delete.txt';source=open_file(deleted,flags=0x04000000)
        check(source!=invalid,'Create delete-on-close file')
        copy=w.HANDLE();check(k.DuplicateHandle(process,source,process,c.byref(copy),0,False,2),'Duplicate delete-on-close file');handles.append(copy.value)
        check(close(source) and k.GetFileAttributesW(str(deleted))!=0xffffffff,'Delete waits for last duplicated handle')
        check(k.WriteFile(copy,c.create_string_buffer(b'x'),1,c.byref(n),None),'Remaining delete-on-close handle still works')
        check(close(copy.value) and not deleted.exists(),'Final duplicate close deletes file')
    finally:
        for h in handles:
            k.SetHandleInformation(h,2,0);k.CloseHandle(h)
print(f'Passed {passed} native file-handle contracts; bridge C++ and guest execution were not built or run.')
