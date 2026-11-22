"""Native aligned CRT allocation contracts. Does not build or run bridge C++."""
import ctypes as c
crt=c.CDLL('ucrtbase.dll')
def fn(name,args,ret=c.c_void_p):
 f=getattr(crt,name);f.argtypes=args;f.restype=ret;return f
allocate=fn('_aligned_offset_malloc',[c.c_size_t]*3)
resize=fn('_aligned_offset_realloc',[c.c_void_p,c.c_size_t,c.c_size_t,c.c_size_t])
recalloc=fn('_aligned_offset_recalloc',[c.c_void_p,c.c_size_t,c.c_size_t,c.c_size_t,c.c_size_t])
release=fn('_aligned_free',[c.c_void_p],None)
msize=fn('_aligned_msize',[c.c_void_p,c.c_size_t,c.c_size_t],c.c_size_t)
geterrno=fn('_errno',[],c.POINTER(c.c_int))
Handler=c.CFUNCTYPE(None,c.c_void_p,c.c_void_p,c.c_void_p,c.c_uint,c.c_size_t)
hits=[]
@Handler
def invalid(*args):hits.append(1)
sethandler=fn('_set_thread_local_invalid_parameter_handler',[c.c_void_p])
old=sethandler(c.cast(invalid,c.c_void_p));checks=0
try:
 for alignment in [1,2,4,8,16,64,4096]:
  for offset in [0,1,7]:
   pointer=allocate(64,alignment,offset);assert pointer
   try:
    assert (pointer+offset)%alignment==0 and msize(pointer,alignment,offset)>=64
    c.memset(pointer,0x6d,64)
    moved=resize(pointer,128,alignment,offset);assert moved;pointer=moved
    assert c.string_at(pointer,64)==b'm'*64 and (pointer+offset)%alignment==0
    oldsize=msize(pointer,alignment,offset)
    moved=recalloc(pointer,1,oldsize+64,alignment,offset);assert moved;pointer=moved
    assert c.string_at(pointer,64)==b'm'*64 and c.string_at(pointer+oldsize,64)==bytes(64)
    checks+=1
   finally:release(pointer)
 for size,alignment,offset in [(0,16,0),(0,1,0)]:
  pointer=allocate(size,alignment,offset);assert pointer;release(pointer);checks+=1
 for args in [(16,0,0),(16,3,0),(16,16,16),(0,16,1)]:
  count=len(hits);geterrno()[0]=0
  assert not allocate(*args) and len(hits)==count+1 and geterrno()[0]==22;checks+=1
 maximum=c.c_size_t(-1).value
 pointer=allocate(32,16,0);assert pointer
 try:
  c.memset(pointer,0x3e,32)
  count=len(hits)
  assert not resize(pointer,maximum,16,0) and c.string_at(pointer,32)==b'>'*32 and len(hits)==count;checks+=1
  assert not recalloc(pointer,maximum,2,16,0) and c.string_at(pointer,32)==b'>'*32 and len(hits)==count;checks+=1
  # A nonnull block with zero requested size is freed before alignment checks.
  assert not resize(pointer,0,3,999) and len(hits)==count;pointer=None;checks+=1
 finally:
  if pointer:release(pointer)
finally:sethandler(old)
print(f'PASS: {checks} native aligned-allocation contracts. Bridge C++ not built/executed.')
