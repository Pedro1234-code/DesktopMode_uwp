"""Native UCRT heap-policy reference probes; no Bridge build/execution."""
import ctypes as c
crt=c.CDLL('ucrtbase.dll')
Handler=c.CFUNCTYPE(c.c_int,c.c_size_t)
for name,args,result in [('_set_new_handler',[c.c_void_p],c.c_void_p),('_query_new_handler',[],c.c_void_p),('_set_new_mode',[c.c_int],c.c_int),('_query_new_mode',[],c.c_int),('_callnewh',[c.c_size_t],c.c_int),('malloc',[c.c_size_t],c.c_void_p),('calloc',[c.c_size_t,c.c_size_t],c.c_void_p),('realloc',[c.c_void_p,c.c_size_t],c.c_void_p),('_recalloc',[c.c_void_p,c.c_size_t,c.c_size_t],c.c_void_p),('free',[c.c_void_p],None),('_msize',[c.c_void_p],c.c_size_t),('_errno',[],c.POINTER(c.c_int))]:
    f=getattr(crt,name);f.argtypes=args;f.restype=result
calls=[]
@Handler
def recover(size):calls.append(size);return 7
@Handler
def reject(size):calls.append(size);return 0
previous=crt._query_new_handler();previous_mode=crt._query_new_mode();blocks=[];checks=0
try:
    assert crt._set_new_handler(None)==previous
    assert crt._callnewh(9)==0;checks+=1
    assert crt._set_new_handler(c.cast(recover,c.c_void_p)) is None
    assert crt._query_new_handler()==c.cast(recover,c.c_void_p).value;checks+=1
    assert crt._callnewh(123)==1 and calls==[123];checks+=1
    crt._set_new_handler(c.cast(reject,c.c_void_p))
    assert crt._callnewh(9)==0 and calls[-1]==9;checks+=1
    crt._set_new_handler(c.cast(recover,c.c_void_p))
    crt._set_new_mode(1);assert crt._query_new_mode()==1;checks+=1
    maximum=c.c_size_t(-1).value
    count=len(calls)
    for name,args in [('malloc',(maximum,)),('calloc',(2,maximum))]:
        crt._errno()[0]=0
        assert not getattr(crt,name)(*args) and crt._errno()[0]==12
        assert len(calls)==count;checks+=1
    for name,args in [('malloc',(0,)),('calloc',(0,maximum)),('realloc',(None,0))]:
        block=getattr(crt,name)(*args);assert block;blocks.append(block);checks+=1
    block=crt.calloc(1,32);assert block;blocks.append(block)
    assert c.string_at(block,32)==bytes(32);checks+=1
    c.memset(block,0x5a,32)
    assert not crt.realloc(block,maximum) and c.string_at(block,32)==b'Z'*32;checks+=1
    old_size=crt._msize(block)
    grown=crt._recalloc(block,1,old_size+64);assert grown;blocks.remove(block);blocks.append(grown)
    assert c.string_at(grown,32)==b'Z'*32 and c.string_at(grown+old_size,64)==bytes(64);checks+=1
    assert not crt.realloc(grown,0);blocks.remove(grown);checks+=1
    block=crt._recalloc(None,8,4);assert block;blocks.append(block)
    assert c.string_at(block,32)==bytes(32);checks+=1
    assert not crt._recalloc(block,0,4);blocks.remove(block);checks+=1
finally:
    for block in blocks:crt.free(block)
    crt._set_new_mode(previous_mode);crt._set_new_handler(previous)
print(f'PASS: {checks} native UCRT heap contracts. Bridge fixtures not built/executed.')
