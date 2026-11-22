"""Native anonymous-pipe contracts. Does not compile or execute bridge C++."""
import ctypes as c
from ctypes import wintypes as w
k=c.WinDLL('kernel32',use_last_error=True)
class Security(c.Structure):
    _fields_=[('length',w.DWORD),('descriptor',c.c_void_p),('inherit',w.BOOL)]
k.CreatePipe.argtypes=[c.POINTER(w.HANDLE),c.POINTER(w.HANDLE),c.POINTER(Security),w.DWORD];k.CreatePipe.restype=w.BOOL
k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
k.GetFileType.argtypes=[w.HANDLE];k.GetFileType.restype=w.DWORD
k.GetHandleInformation.argtypes=[w.HANDLE,c.POINTER(w.DWORD)];k.GetHandleInformation.restype=w.BOOL
k.SetHandleInformation.argtypes=[w.HANDLE,w.DWORD,w.DWORD];k.SetHandleInformation.restype=w.BOOL
k.DuplicateHandle.argtypes=[w.HANDLE,w.HANDLE,w.HANDLE,c.POINTER(w.HANDLE),w.DWORD,w.BOOL,w.DWORD];k.DuplicateHandle.restype=w.BOOL
k.ReadFile.argtypes=[w.HANDLE,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.c_void_p];k.ReadFile.restype=w.BOOL
k.WriteFile.argtypes=k.ReadFile.argtypes;k.WriteFile.restype=w.BOOL
k.PeekNamedPipe.argtypes=[w.HANDLE,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.POINTER(w.DWORD),c.POINTER(w.DWORD)];k.PeekNamedPipe.restype=w.BOOL
passed=0; handles=[]; process=w.HANDLE(-1)
def check(ok,label):
    global passed
    if not ok: raise AssertionError(label)
    passed+=1
try:
    for inherit in (False,True):
        a,b=w.HANDLE(),w.HANDLE(); flags=w.DWORD();security=Security(c.sizeof(Security),None,inherit)
        check(k.CreatePipe(c.byref(a),c.byref(b),c.byref(security),128),'Create anonymous pipe')
        handles.extend([a,b]); check(k.GetFileType(a)==3 and k.GetFileType(b)==3,'Both endpoints have FILE_TYPE_PIPE')
        check(k.GetHandleInformation(a,c.byref(flags)) and flags.value==int(inherit),'Creation inherit flag')
        check(k.SetHandleInformation(a,1,0) and k.GetHandleInformation(a,c.byref(flags)) and not flags.value&1,'Clear inheritance independently')
        n=w.DWORD(); available=w.DWORD(); left=w.DWORD(); buffer=c.create_string_buffer(32)
        check(k.ReadFile(b,buffer,1,c.byref(n),None)==0 and c.get_last_error()==5,'Read on writer reports access denied')
        check(k.WriteFile(a,buffer,1,c.byref(n),None)==0 and c.get_last_error()==5,'Write on reader reports access denied')
        check(k.PeekNamedPipe(a,None,0,c.byref(n),c.byref(available),c.byref(left)) and n.value==available.value==left.value==0,'Peek empty open pipe does not block')
        payload=c.create_string_buffer(b'abcdef')
        check(k.WriteFile(b,payload,6,c.byref(n),None) and n.value==6,'Write all bytes')
        ok=k.PeekNamedPipe(a,buffer,2,c.byref(n),c.byref(available),c.byref(left))
        check(ok and n.value==2 and available.value==6 and buffer.raw[:2]==b'ab','Peek does not consume bytes')
        duplicate=w.HANDLE()
        check(k.DuplicateHandle(process,b,process,c.byref(duplicate),0,False,2),'Duplicate writer')
        handles.append(duplicate)
        check(k.GetHandleInformation(duplicate,c.byref(flags)) and flags.value==0,'Duplicate has independent inherit flags')
        check(k.CloseHandle(b),'Close original writer');handles.remove(b)
        check(k.ReadFile(a,buffer,3,c.byref(n),None) and n.value==3 and buffer.raw[:3]==b'abc','Read partial prefix')
        check(k.CloseHandle(duplicate),'Close final writer');handles.remove(duplicate)
        check(k.ReadFile(a,buffer,32,c.byref(n),None) and n.value==3 and buffer.raw[:3]==b'def','Drain data after writer exit')
        check(not k.ReadFile(a,buffer,1,c.byref(n),None) and c.get_last_error()==109 and n.value==0,'Read EOF is ERROR_BROKEN_PIPE')
        check(not k.ReadFile(a,None,0,c.byref(n),None) and c.get_last_error()==109,'Zero-byte read still reports EOF')
        check(not k.PeekNamedPipe(a,None,0,None,c.byref(available),None) and c.get_last_error()==109,'Peek EOF reports broken pipe')
        k.CloseHandle(a);handles.remove(a)
    a,b=w.HANDLE(),w.HANDLE();check(k.CreatePipe(c.byref(a),c.byref(b),None,0),'Create another pipe');handles.extend([a,b])
    check(k.SetHandleInformation(b,2,2),'Protect writer from close')
    check(not k.CloseHandle(b) and c.get_last_error()==6,'Protected CloseHandle returns ERROR_INVALID_HANDLE')
    duplicate=w.HANDLE();check(k.DuplicateHandle(process,b,process,c.byref(duplicate),0,False,3),'Duplicate with CLOSE_SOURCE on protected writer succeeds')
    handles.append(duplicate)
    check(k.GetHandleInformation(b,c.byref(flags)) and flags.value&2,'Protected source remains open after duplicate close request')
    k.CloseHandle(a);handles.remove(a)
    for count in (0,1):
        check(not k.WriteFile(duplicate,c.create_string_buffer(b'x'),count,c.byref(n),None) and c.get_last_error()==232,'No readers produces ERROR_NO_DATA even for zero-byte writes')
finally:
    for h in handles:
        k.SetHandleInformation(h,2,0);k.CloseHandle(h)
print(f'Passed {passed} native pipe contracts; bridge C++ and guest process inheritance were not executed.')
