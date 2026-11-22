"""Compare the bridge's explicit NTSTATUS map with native ntdll; no C++ execution."""
import ctypes as c
import re
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'Bridge/NativeGuestShims.inl').read_text(encoding='utf-8-sig')
body=source.split('ULONG WINAPI GuestRtlNtStatusToDosError(LONG status){',1)[1].split('BOOLEAN WINAPI GuestRtlCreateUnicodeStringFromAsciiz',1)[0]
errors=Path('C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared/winerror.h').read_text(encoding='utf-8-sig')
values={name:int(value) for name,value in re.findall(r'#define\s+(ERROR_\w+)\s+(\d+)L?\b',errors)}
translate=c.WinDLL('ntdll').RtlNtStatusToDosError;translate.argtypes=[c.c_long];translate.restype=c.c_ulong
pending=[];checks=0
for token in re.finditer(r'case\s+(0x[0-9a-f]+)u?:|return\s+(ERROR_\w+);',body,re.I):
    if token[1]:pending.append(int(token[1],16))
    elif pending:
        expected=values[token[2]]
        for status in pending:
            actual=translate(c.c_long(status).value)
            assert actual==expected,(hex(status),token[2],expected,actual)
            checks+=1
        pending=[]
for status in (0xc0070005,0xc0070057):
    assert translate(c.c_long(status).value)==status&0xffff;checks+=1
assert checks>=25
print(f'PASS: {checks} source-derived NTSTATUS mappings agree with native ntdll. Bridge C++ not built/executed.')
