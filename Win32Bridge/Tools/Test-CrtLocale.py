"""Native UCRT locale contracts. No Bridge C++ build or execution."""
import ctypes as c,struct,threading,subprocess,sys,tempfile
from pathlib import Path
m=c.CDLL('ucrtbase.dll')
def fn(name,args,result):
 f=getattr(m,name);f.argtypes=args;f.restype=result;return f
configure=fn('_configthreadlocale',[c.c_int],c.c_int)
locale=fn('_wsetlocale',[c.c_int,c.c_wchar_p],c.c_wchar_p)
create=fn('_wcreate_locale',[c.c_int,c.c_wchar_p],c.c_void_p)
free=fn('_free_locale',[c.c_void_p],None)
snapshot=fn('_get_current_locale',[],c.c_void_p)
conv=fn('localeconv',[],c.c_void_p)
parse=fn('strtod',[c.c_char_p,c.POINTER(c.c_char_p)],c.c_double)
parse_l=fn('_strtod_l',[c.c_char_p,c.POINTER(c.c_char_p),c.c_void_p],c.c_double)
format_buffer=fn('__stdio_common_vsprintf',[c.c_uint64,c.c_void_p,c.c_size_t,c.c_char_p,c.c_void_p,c.c_void_p],c.c_int)
scan_buffer=fn('__stdio_common_vsscanf',[c.c_uint64,c.c_char_p,c.c_size_t,c.c_char_p,c.c_void_p,c.c_void_p],c.c_int)
geterrno=fn('_errno',[],c.POINTER(c.c_int))
old_mode=configure(0);old_name=locale(0,None);checks=0;lease=None;fr=None
try:
 configure(2);assert locale(0,'C')=='C';checks+=1
 assert parse(b'1,5',None)==1.0;checks+=1
 canonical=locale(4,'fr-FR');assert canonical and parse(b'1,5',None)==1.5;checks+=1
 aggregate=locale(0,None);assert 'LC_NUMERIC=' in aggregate;checks+=1
 assert locale(0,'C')=='C' and locale(0,aggregate)==aggregate;checks+=1
 lease=snapshot();assert lease;checks+=1
 pointer=c.c_void_p.from_address(conv()).value;assert c.string_at(pointer)==b',';checks+=1
 locale(0,'C');assert c.string_at(pointer)==b',' and parse_l(b'1,5',None,lease)==1.5;checks+=1
 fr=create(0,'fr-FR');assert fr;checks+=1
 output=c.create_string_buffer(32);bits=struct.unpack('<Q',struct.pack('<d',1.5))[0];args=(c.c_uint64*1)(bits)
 assert format_buffer(0,output,len(output),b'%.1f',fr,c.addressof(args))==3 and output.value==b'1,5';checks+=1
 number=c.c_double();pointers=(c.c_uint64*1)(c.addressof(number))
 assert scan_buffer(0,b'1,5',3,b'%lf',fr,c.addressof(pointers))==1 and number.value==1.5;checks+=1
 assert parse(b'1,5',None)==1.0;checks+=1
 assert configure(1)==2;assert locale(4,'fr-FR');assert configure(0)==1;checks+=1
 assert parse(b'1,5',None)==1.5;checks+=1
 assert configure(2)==1 and parse(b'1,5',None)==1.0;checks+=1
 ready=threading.Event();changed=threading.Event();result=[]
 def worker():
  configure(1);locale(0,'C');ready.set();changed.wait()
  result.append(parse(b'1,5',None));configure(2);result.append(parse(b'1,5',None))
 thread=threading.Thread(target=worker);thread.start();assert ready.wait(5)
 assert locale(4,'fr-FR');changed.set();thread.join(5);assert not thread.is_alive() and result==[1.0,1.5];checks+=1
 inherited=[]
 def inherit():inherited.append(parse(b'1,5',None))
 thread=threading.Thread(target=inherit);thread.start();thread.join();assert inherited==[1.5];checks+=1
 before=locale(0,None);assert not locale(4,'no-such-locale-987654') and locale(0,None)==before;checks+=1
 Handler=c.CFUNCTYPE(None,c.c_void_p,c.c_void_p,c.c_void_p,c.c_uint,c.c_size_t)
 hits=[]
 @Handler
 def invalid(*args):hits.append(1)
 change=fn('_set_thread_local_invalid_parameter_handler',[c.c_void_p],c.c_void_p);previous=change(c.cast(invalid,c.c_void_p))
 try:
  assert not locale(999,'C') and geterrno()[0]==22 and len(hits)==1;checks+=1
  assert configure(7)==-1 and geterrno()[0]==22 and len(hits)==2;checks+=1
 finally:change(previous)
 configure(2);locale(0,'C');barrier=threading.Barrier(2);updates=[]
 def category_update(category,name):
  configure(2);barrier.wait(timeout=5);updates.append(bool(locale(category,name)))
 left=threading.Thread(target=category_update,args=(2,'en-US'));right=threading.Thread(target=category_update,args=(4,'fr-FR'))
 left.start();right.start();left.join(5);right.join(5)
 assert not left.is_alive() and not right.is_alive() and updates==[True,True]
 assert locale(2,None)!='C' and parse(b'1,5',None)==1.5;checks+=1
 # Explicit handles must preserve caller PTD identity, including borrowed
 # names/lconv pointers from a distinct non-C private locale.
 configure(1);assert locale(0,'fr-FR');checks+=1
 raw_locale=c.CFUNCTYPE(c.c_void_p,c.c_int,c.c_wchar_p)(c.cast(locale,c.c_void_p).value)
 name_pointer=raw_locale(0,None);host_name=c.wstring_at(name_pointer);conv_pointer=conv()
 def identity():
  value=snapshot();assert value
  pair=tuple((c.c_void_p*2).from_address(value));free(value);return pair
 host_identity=identity();private=[];worker_failures=[]
 setmbcp=fn('_setmbcp',[c.c_int],c.c_int)
 lower_l=fn('_towlower_l',[c.c_ushort,c.c_void_p],c.c_ushort)
 def build_private():
  try:
   assert configure(1)>0 and locale(0,'C')=='C' and setmbcp(0)==0
   assert locale(2,'tr-TR') and locale(4,'fr-FR')
   names=[locale(i,None) for i in range(6)];handle=snapshot();assert handle
   private.append((handle,names,conv()))
  except BaseException as error:worker_failures.append(error)
 thread=threading.Thread(target=build_private);thread.start();thread.join(5)
 assert not thread.is_alive() and not worker_failures and len(private)==1;checks+=1
 handle,names,private_conv=private[0]
 try:
  assert 'LC_CTYPE=' in names[0] and 'LC_NUMERIC=' in names[0];checks+=1
  assert c.string_at(c.c_void_p.from_address(private_conv).value)==b',';checks+=1
  assert parse_l(b'1,5',None,handle)==1.5 and lower_l(0xD6,handle)==0xF6;checks+=1
  args=(c.c_uint64*1)(struct.unpack('<Q',struct.pack('<d',1.5))[0])
  assert format_buffer(0,output,len(output),b'%.1f',handle,c.addressof(args))==3 and output.value==b'1,5';checks+=1
  number=c.c_double();pointers=(c.c_uint64*1)(c.addressof(number))
  assert scan_buffer(0,b'1,5',3,b'%lf',handle,c.addressof(pointers))==1 and number.value==1.5;checks+=1
  assert identity()==host_identity and c.wstring_at(name_pointer)==host_name and conv()==conv_pointer;checks+=1
  # File scanf with a valid native FILE uses the same explicit decimal locale.
  fopen=fn('_wfopen_s',[c.POINTER(c.c_void_p),c.c_wchar_p,c.c_wchar_p],c.c_int)
  fclose=fn('fclose',[c.c_void_p],c.c_int)
  fscan=fn('__stdio_common_vfscanf',[c.c_uint64,c.c_void_p,c.c_char_p,c.c_void_p,c.c_void_p],c.c_int)
  ftell=fn('ftell',[c.c_void_p],c.c_long)
  with tempfile.TemporaryDirectory() as directory:
   path=Path(directory)/'locale.txt';path.write_bytes(b'1,5 X');stream=c.c_void_p()
   assert fopen(c.byref(stream),str(path),'rb')==0 and stream.value
   try:
    number=c.c_double();used=c.c_int(-1);pointers=(c.c_uint64*2)(c.addressof(number),c.addressof(used))
    assert fscan(0,stream,b'%lf%n',handle,c.addressof(pointers))==1 and number.value==1.5;checks+=1
    assert used.value==3 and ftell(stream)==3;checks+=1
   finally:assert fclose(stream)==0
  # A factory round trip of the compound canonical name retains both categories.
  roundtrip=create(0,names[0]);assert roundtrip
  try:assert parse_l(b'1,5',None,roundtrip)==1.5 and lower_l(0xD6,roundtrip)==0xF6;checks+=1
  finally:free(roundtrip)
 finally:free(handle)
 assert identity()==host_identity and c.wstring_at(name_pointer)==host_name and conv()==conv_pointer;checks+=1
finally:
 if fr:free(fr)
 if lease:free(lease)
 configure(2);locale(0,old_name);configure(old_mode)
# Startup mode -1 is irreversible in the native CRT: probe in a fresh child.
startup_probe=r"""
import ctypes as c,threading
m=c.CDLL('ucrtbase.dll')
f=m._configthreadlocale;f.argtypes=[c.c_int];f.restype=c.c_int
s=m._wsetlocale;s.argtypes=[c.c_int,c.c_wchar_p];s.restype=c.c_wchar_p
p=m.strtod;p.argtypes=[c.c_char_p,c.c_void_p];p.restype=c.c_double
f(2);assert s(0,'C')=='C';assert f(-1)==2;assert s(4,'fr-FR')
assert f(0)==2 and p(b'1,5',None)==1.5
observed=[]
def worker():observed.append(p(b'1,5',None))
t=threading.Thread(target=worker);t.start();t.join();assert observed==[1.0]
"""
child=subprocess.run([sys.executable,'-c',startup_probe],capture_output=True,text=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
assert child.returncode==0,child.stderr
checks+=1
print(f'PASS: {checks} native locale/global/private/thread/lease/format/scan contracts. Bridge fixtures not built/executed.')
