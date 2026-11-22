"""Native MSVC exception_ptr/handler ABI probes. No bridge build or execution."""
import ctypes as c,struct,threading
stl=c.CDLL('msvcp140.dll');vcrt=c.CDLL('vcruntime140.dll');ucrt=c.CDLL('ucrtbase.dll')
checks=0

def function(module,name,args,result=None):
 f=getattr(module,name);f.argtypes=args;f.restype=result;return f
class Ptr(c.Structure):_fields_=[('record',c.c_void_p),('control',c.c_void_p)]
class Record(c.Structure):
 _fields_=[('code',c.c_uint32),('flags',c.c_uint32),('nested',c.c_void_p),('address',c.c_void_p),('count',c.c_uint32),('parameters',c.c_size_t*15)]
create=function(stl,'?__ExceptionPtrCreate@@YAXPEAX@Z',[c.POINTER(Ptr)])
destroy=function(stl,'?__ExceptionPtrDestroy@@YAXPEAX@Z',[c.POINTER(Ptr)])
copy=function(stl,'?__ExceptionPtrCopy@@YAXPEAXPEBX@Z',[c.POINTER(Ptr),c.POINTER(Ptr)])
assign=function(stl,'?__ExceptionPtrAssign@@YAXPEAXPEBX@Z',[c.POINTER(Ptr),c.POINTER(Ptr)])
compare=function(stl,'?__ExceptionPtrCompare@@YA_NPEBX0@Z',[c.POINTER(Ptr),c.POINTER(Ptr)],c.c_bool)
tobool=function(stl,'?__ExceptionPtrToBool@@YA_NPEBX@Z',[c.POINTER(Ptr)],c.c_bool)
swap=function(stl,'?__ExceptionPtrSwap@@YAXPEAX0@Z',[c.POINTER(Ptr),c.POINTER(Ptr)])
capture=function(stl,'?__ExceptionPtrCurrentException@@YAXPEAX@Z',[c.POINTER(Ptr)])
slot=function(vcrt,'__current_exception',[],c.POINTER(c.c_void_p))()
assert c.sizeof(Ptr)==16 and c.sizeof(Record)==152;checks+=1
first,second,third=Ptr(),Ptr(),Ptr();create(c.byref(first));create(c.byref(second));create(c.byref(third))
try:
 assert not tobool(c.byref(first)) and compare(c.byref(first),c.byref(second));checks+=1
 capture(c.byref(first));assert not tobool(c.byref(first));checks+=1
 # Minimal simple-type x64 ThrowInfo, with image-relative CatchableType RVA.
 image=c.create_string_buffer(4096);base=c.addressof(image)
 c.memmove(base,struct.pack('<4i',0,0,0,0x40),16)
 c.memmove(base+0x40,struct.pack('<2i',1,0x60),8)
 c.memmove(base+0x60,struct.pack('<7i',1,0x80,0,-1,0,4,0),28)
 c.memmove(base+0x90,b'.H\0',3)
 value=c.c_int(345);record=Record();record.code=0xe06d7363;record.flags=1;record.count=4
 record.parameters[0]=0x19930520;record.parameters[1]=c.addressof(value);record.parameters[2]=base;record.parameters[3]=base
 previous=slot[0]
 try:slot[0]=c.addressof(record);capture(c.byref(first))
 finally:slot[0]=previous
 assert tobool(c.byref(first));checks+=1
 retained=c.cast(first.record,c.POINTER(Record)).contents
 assert retained.parameters[3]==base and retained.parameters[1]!=c.addressof(value);checks+=1
 value.value=999;assert c.c_int.from_address(retained.parameters[1]).value==345;checks+=1
 assign(c.byref(second),c.byref(first));assert compare(c.byref(first),c.byref(second));checks+=1
 assign(c.byref(second),c.byref(second));assert compare(c.byref(first),c.byref(second));checks+=1
 destroy(c.byref(third));copy(c.byref(third),c.byref(first));assert compare(c.byref(first),c.byref(third));checks+=1
 observed=[]
 def worker():
  local=Ptr();copy(c.byref(local),c.byref(first))
  try:observed.append(compare(c.byref(local),c.byref(first)))
  finally:destroy(c.byref(local))
 t=threading.Thread(target=worker);t.start();t.join();assert observed==[True];checks+=1
 destroy(c.byref(second));create(c.byref(second));swap(c.byref(first),c.byref(second))
 assert not tobool(c.byref(first)) and compare(c.byref(second),c.byref(third));checks+=1
finally:
 destroy(c.byref(first));destroy(c.byref(second));destroy(c.byref(third))
Handler=c.CFUNCTYPE(None,c.c_wchar_p,c.c_wchar_p,c.c_wchar_p,c.c_uint,c.c_size_t)
setglobal=function(ucrt,'_set_invalid_parameter_handler',[c.c_void_p],c.c_void_p)
setlocal=function(ucrt,'_set_thread_local_invalid_parameter_handler',[c.c_void_p],c.c_void_p)
invalid=function(ucrt,'_invalid_parameter_noinfo',[])
events=[]
@Handler
def global_handler(*args):events.append(('global',args))
@Handler
def local_handler(*args):events.append(('local',args))
previous_global=setglobal(c.cast(global_handler,c.c_void_p));previous_local=setlocal(c.cast(local_handler,c.c_void_p))
try:
 invalid();assert events[-1]==('local',(None,None,None,0,0));checks+=1
 setlocal(None);invalid();assert events[-1][0]=='global';checks+=1
finally:setlocal(previous_local);setglobal(previous_global)
print(f'PASS: {checks} native MSVC exception_ptr/invalid-parameter contracts. Bridge C++ not built/executed.')
