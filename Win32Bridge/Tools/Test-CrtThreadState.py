"""Native CRT thread-state reference contracts. Bridge C++ is not executed."""
import ctypes as c,threading
m=c.CDLL('ucrtbase.dll')
def fn(name,args,result):
 f=getattr(m,name);f.argtypes=args;f.restype=result;return f
error=fn('_errno',[],c.POINTER(c.c_int));dos=fn('__doserrno',[],c.POINTER(c.c_ulong))
seterror=fn('_set_errno',[c.c_int],c.c_int);geterror=fn('_get_errno',[c.POINTER(c.c_int)],c.c_int)
setdos=fn('_set_doserrno',[c.c_ulong],c.c_int);getdos=fn('_get_doserrno',[c.POINTER(c.c_ulong)],c.c_int)
seed=fn('srand',[c.c_uint],None);random=fn('rand',[],c.c_int)
checks=0;saved_error=error()[0];saved_dos=dos()[0]
try:
 assert seterror(123)==0 and setdos(456)==0
 e=c.c_int();d=c.c_ulong();assert geterror(c.byref(e))==0 and getdos(c.byref(d))==0
 assert e.value==123 and d.value==456;checks+=1
 error()[0]=321;dos()[0]=654
 assert geterror(c.byref(e))==0 and getdos(c.byref(d))==0 and e.value==321 and d.value==654;checks+=1
 main_error=c.addressof(error().contents);main_dos=c.addressof(dos().contents)
 results=[]
 def worker():
  different=c.addressof(error().contents)!=main_error and c.addressof(dos().contents)!=main_dos
  seterror(7);setdos(8);seed(17)
  results.append((different,error()[0],dos()[0],[random() for _ in range(6)]))
 t=threading.Thread(target=worker);t.start();t.join()
 assert results==[(True,7,8,[94,26602,30017,18297,20363,13015])];checks+=1
 assert error()[0]==321 and dos()[0]==654;checks+=1
 expected={0:[38,7719,21238,2437,8855,11797],1:[41,18467,6334,26500,19169,15724],17:[94,26602,30017,18297,20363,13015],0xffffffff:[35,29739,3374,11141,31308,7870]}
 for value,sequence in expected.items():
  seed(value);assert [random() for _ in sequence]==sequence;checks+=1
 Handler=c.CFUNCTYPE(None,c.c_void_p,c.c_void_p,c.c_void_p,c.c_uint,c.c_size_t)
 hits=[]
 @Handler
 def invalid(*args):hits.append(1)
 change=fn('_set_thread_local_invalid_parameter_handler',[c.c_void_p],c.c_void_p)
 previous=change(c.cast(invalid,c.c_void_p))
 try:
  seterror(99);setdos(100)
  assert geterror(None)==22 and error()[0]==99 and len(hits)==1;checks+=1
  assert getdos(None)==22 and error()[0]==99 and dos()[0]==100 and len(hits)==2;checks+=1
 finally:change(previous)
finally:setdos(saved_dos);seterror(saved_error)
print(f'PASS: {checks} native errno/doserrno/rand thread contracts. Bridge fixtures not built/executed.')
