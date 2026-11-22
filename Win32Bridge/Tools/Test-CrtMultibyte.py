"""Native UCRT MBCS reference contracts; does not build or execute Bridge C++."""
import ctypes as c
import threading
import subprocess
import sys

u=c.CDLL('ucrtbase.dll')
def fn(name,args,result):
    function=getattr(u,name);function.argtypes=args;function.restype=result;return function
configure=fn('_configthreadlocale',[c.c_int],c.c_int)
locale=fn('_wsetlocale',[c.c_int,c.c_wchar_p],c.c_wchar_p)
setcp=fn('_setmbcp',[c.c_int],c.c_int)
getcp=fn('_getmbcp',[],c.c_int)
snapshot=fn('_get_current_locale',[],c.c_void_p)
free=fn('_free_locale',[c.c_void_p],None)
lead=fn('_ismbblead',[c.c_uint],c.c_int)
lead_l=fn('_ismbblead_l',[c.c_uint,c.c_void_p],c.c_int)
trail_l=fn('_ismbbtrail_l',[c.c_uint,c.c_void_p],c.c_int)
upper=fn('_mbctoupper',[c.c_uint],c.c_uint)
upper_l=fn('_mbctoupper_l',[c.c_uint,c.c_void_p],c.c_uint)
length_l=fn('_mbslen_l',[c.c_void_p,c.c_void_p],c.c_size_t)
inc_l=fn('_mbsinc_l',[c.c_void_p,c.c_void_p],c.c_void_p)
nbcnt_l=fn('_mbsnbcnt_l',[c.c_void_p,c.c_size_t,c.c_void_p],c.c_size_t)
nccnt_l=fn('_mbsnccnt_l',[c.c_void_p,c.c_size_t,c.c_void_p],c.c_size_t)
copy_l=fn('_mbsncpy_s_l',[c.c_void_p,c.c_size_t,c.c_void_p,c.c_size_t,c.c_void_p],c.c_int)
compare_l=fn('_mbscmp_l',[c.c_void_p,c.c_void_p,c.c_void_p],c.c_int)
mbtowc_l=fn('_mbtowc_l',[c.POINTER(c.c_wchar),c.c_char_p,c.c_size_t,c.c_void_p],c.c_int)
wctomb_l=fn('_wctomb_s_l',[c.POINTER(c.c_int),c.c_void_p,c.c_size_t,c.c_wchar,c.c_void_p],c.c_int)
geterrno=fn('_errno',[],c.POINTER(c.c_int))
checks=0
configure(1);assert locale(0,'C')=='C'
for cp,reported in [(0,0),(1251,0),(1252,0),(932,932),(936,936),(949,949),(950,950),(65001,0)]:
    assert setcp(cp)==0 and getcp()==reported;checks+=1
assert setcp(932)==0 and lead(0x82);checks+=1
value=snapshot();assert value
try:
    assert lead_l(0x82,value) and trail_l(0xA0,value);checks+=1
    assert setcp(0)==0 and not lead(0x82) and lead_l(0x82,value);checks+=1
    data=c.create_string_buffer(bytes([0x82,0xA0,0x41,0]))
    assert length_l(data,value)==2 and inc_l(data,value)==c.addressof(data)+2;checks+=1
    assert nbcnt_l(data,1,value)==2 and nccnt_l(data,2,value)==1;checks+=1
    target=c.create_string_buffer(16)
    assert copy_l(target,len(target),data,1,value)==0 and target.value==bytes([0x82,0xA0]);checks+=1
    assert compare_l(data,target,value)>0;checks+=1
    # LC_CTYPE stays C while MBCS is Japanese: mbtowc uses LC_CTYPE, not MBCS.
    wide=c.c_wchar()
    assert mbtowc_l(c.byref(wide),bytes([0x82,0xA0]),2,value)==1 and ord(wide.value)==0x82;checks+=1
finally:free(value)
assert locale(2,'Japanese_Japan.932') and setcp(0)==0;checks+=1
value=snapshot()
try:
    wide=c.c_wchar();assert not lead_l(0x82,value) and mbtowc_l(c.byref(wide),bytes([0x82,0xA0]),2,value)==2 and ord(wide.value)==0x3042;checks+=1
    count=c.c_int();target=c.create_string_buffer(8)
    assert wctomb_l(c.byref(count),target,len(target),chr(0x3042),value)==0 and count.value==2 and target.raw[:2]==bytes([0x82,0xA0]);checks+=1
finally:free(value)
assert setcp(-4)==0 and getcp()==932;checks+=1
assert locale(2,'C')=='C' and getcp()==932;checks+=1
geterrno()[0]=0;assert setcp(65000)==-1 and geterrno()[0]==22 and getcp()==932;checks+=1
geterrno()[0]=0;assert setcp(-1)==-1 and geterrno()[0]==22 and getcp()==932;checks+=1
assert setcp(65001)==0 and getcp()==0 and not lead(0xC3);checks+=1
assert setcp(1251)==0 and getcp()==0 and upper(0xFF)==0xDF;checks+=1
assert setcp(1252)==0 and getcp()==0 and upper(0xFF)==0x9F;checks+=1
# A snapshot returned by a finished private helper owns its MBCS reference.
owned=[]
def helper():
    configure(1);assert locale(0,'C')=='C';assert setcp(932)==0;owned.append(snapshot())
t=threading.Thread(target=helper);t.start();t.join(5)
assert not t.is_alive() and len(owned)==1 and owned[0];checks+=1
try:assert lead_l(0x82,owned[0]) and not lead(0x82);checks+=1
finally:free(owned[0])
# Shared/private modes affect MBCS independently of category strings.
configure(2);assert locale(0,'C')=='C' and setcp(932)==0
ready=threading.Event();resume=threading.Event();observed=[]
def private_worker():
    configure(1);observed.append(getcp());assert setcp(0)==0;ready.set();resume.wait(5)
    observed.append(getcp());configure(2);observed.append(getcp())
t=threading.Thread(target=private_worker);t.start();assert ready.wait(5)
assert setcp(936)==0;resume.set();t.join(5)
assert not t.is_alive() and observed==[0,0,936] and getcp()==936;checks+=1
# Reading shared state before enabling private mode captures the shared page.
observed=[]
def synchronized_worker():
    observed.append(getcp());configure(1);observed.append(getcp())
t=threading.Thread(target=synchronized_worker);t.start();t.join(5)
assert not t.is_alive() and observed==[936,936];checks+=1
# Explicit handles leave caller locale and MBCS pointer identity unchanged.
configure(1);assert setcp(1251)==0
value=snapshot();before=tuple((c.c_void_p*2).from_address(value))
try:
    owned=[];t=threading.Thread(target=helper);t.start();t.join(5)
    explicit=owned.pop()
    try:
        assert upper_l(0xFF,explicit)==0xFF and lead_l(0x82,explicit);checks+=1
        after=snapshot()
        try:assert tuple((c.c_void_p*2).from_address(after))==before and upper(0xFF)==0xDF;checks+=1
        finally:free(after)
    finally:free(explicit)
finally:free(value)
# Startup -1 changes process-native flags irreversibly, so probe a child.
startup=r"""
import ctypes as c,threading
u=c.CDLL('ucrtbase.dll');cfg=u._configthreadlocale;cfg.argtypes=[c.c_int]
s=u._setmbcp;s.argtypes=[c.c_int];g=u._getmbcp;g.restype=c.c_int
cfg(2);assert s(932)==0;assert cfg(-1)==2;assert s(936)==0 and g()==936
seen=[]
def worker():seen.append(g());assert s(1251)==0;seen.append(g())
t=threading.Thread(target=worker);t.start();t.join();assert seen==[0,0] and g()==936
"""
child=subprocess.run([sys.executable,'-c',startup],capture_output=True,text=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
assert child.returncode==0,child.stderr
checks+=1
print(f'PASS: {checks} native UCRT MBCS/locale/snapshot/thread contracts. Bridge fixtures not built/executed.')
