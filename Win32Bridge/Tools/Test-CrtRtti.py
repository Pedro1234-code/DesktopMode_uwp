"""Native MSVC x64 RTTI relative-metadata reference probes. No bridge execution."""
import ctypes as c,struct
crt=c.CDLL('vcruntime140.dll')
identify=crt.__RTtypeid;identify.argtypes=[c.c_void_p];identify.restype=c.c_void_p
complete=crt.__RTCastToVoid;complete.argtypes=[c.c_void_p];complete.restype=c.c_void_p
cast=crt.__RTDynamicCast;cast.argtypes=[c.c_void_p,c.c_long,c.c_void_p,c.c_void_p,c.c_int];cast.restype=c.c_void_p
image=c.create_string_buffer(4096);base=c.addressof(image)
vtable=(c.c_void_p*2)(base+0x40,None)
# Complete-object locator revision 1 carries its own RVA. No host module entry.
c.memmove(base+0x40,struct.pack('<6I',1,8,0,0x80,0x100,0x40),24)
c.memmove(base+0x90,b'.?AVRelativeFixture@@\0',22)
object_storage=c.create_string_buffer(32);object_base=c.addressof(object_storage)
c.c_void_p.from_address(object_base+8).value=c.addressof(vtable)+c.sizeof(c.c_void_p)
assert identify(object_base+8)==base+0x80
assert complete(object_base+8)==object_base
assert not cast(None,0,None,None,0)
print('PASS: 3 native RTTI contracts, including self-relative metadata without host module registration. Bridge C++ not built/executed.')
