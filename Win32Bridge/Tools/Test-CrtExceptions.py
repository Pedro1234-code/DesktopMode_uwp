"""Native MSVC helper contracts; does not build or execute bridge C++ code."""
import ctypes as c
import threading

if c.sizeof(c.c_void_p) != 8:
    raise SystemExit("Windows x64 ctypes ABI required")
vcrt = c.CDLL("vcruntime140.dll")
fh4 = c.CDLL("vcruntime140_1.dll")
ucrt = c.CDLL("ucrtbase.dll")
passed = 0

def check(condition, label):
    global passed
    if not condition:
        raise AssertionError(label)
    passed += 1

class ExceptionData(c.Structure):
    _fields_ = [("what", c.c_void_p), ("owned", c.c_bool)]

copy = vcrt.__std_exception_copy
copy.argtypes = [c.POINTER(ExceptionData), c.POINTER(ExceptionData)]; copy.restype = None
destroy = vcrt.__std_exception_destroy
destroy.argtypes = [c.POINTER(ExceptionData)]; destroy.restype = None
message = c.create_string_buffer(b"guest exception text")
source, target = ExceptionData(c.addressof(message), False), ExceptionData()
copy(c.byref(source), c.byref(target))
check(target.what == source.what and not target.owned, "Borrowed exception message")
destroy(c.byref(target))
check(not target.what and not target.owned and message.value == b"guest exception text", "Borrowed message destruction")
source.owned = True
copy(c.byref(source), c.byref(target))
try:
    check(target.what and target.what != source.what and target.owned and c.string_at(target.what) == message.value, "Owned exception message copy")
finally:
    destroy(c.byref(target))
check(not target.what and not target.owned, "Owned message destruction clears data")
source.what = None
copy(c.byref(source), c.byref(target))
check(not target.what and not target.owned, "Empty exception message")

def type_data(name):
    storage = c.create_string_buffer(8 + len(name) + 1)
    c.memmove(c.addressof(storage) + 8, name, len(name) + 1)
    return storage

a, same, other = type_data(b".?AVWidget@@"), type_data(b".?AVWidget@@"), type_data(b".?AVOther@@")
compare = vcrt.__std_type_info_compare
compare.argtypes = [c.c_void_p, c.c_void_p]; compare.restype = c.c_int
hash_type = vcrt.__std_type_info_hash
hash_type.argtypes = [c.c_void_p]; hash_type.restype = c.c_size_t
check(compare(a, same) == 0 and hash_type(a) == hash_type(same), "RTTI identity by mangled name")
check(compare(a, other) != 0 and (compare(a, other) > 0) == (compare(other, a) < 0), "RTTI ordering")
# __type_info_node begins with a 16-byte-aligned SLIST_HEADER on x64.
root_storage = c.create_string_buffer(48)
root = (c.addressof(root_storage) + 15) & ~15
name_type = vcrt.__std_type_info_name
name_type.argtypes = [c.c_void_p, c.c_void_p]; name_type.restype = c.c_void_p
free_names = vcrt.__std_type_info_destroy_list
free_names.argtypes = [c.c_void_p]; free_names.restype = None
try:
    name = name_type(a, root)
    check(name and b"Widget" in c.string_at(name), "RTTI demangling")
    check(name_type(a, root) == name and c.c_void_p.from_buffer(a).value == name, "RTTI name cache")
finally:
    free_names(root)
# The module owning a descriptor must not use its cached pointer after unload.
c.c_void_p.from_buffer(a).value = None

handler_type = c.CFUNCTYPE(None)
@handler_type
def handler():
    raise AssertionError("This probe must never call terminate")
setter, getter = ucrt.set_terminate, ucrt._get_terminate
setter.argtypes = [c.c_void_p]; setter.restype = c.c_void_p
getter.argtypes = []; getter.restype = c.c_void_p
callback = c.cast(handler, c.c_void_p).value
previous = setter(callback)
try:
    check(getter() == callback, "Native terminate handler installed")
    current = vcrt.__current_exception
    context = vcrt.__current_exception_context
    processing = vcrt.__processing_throw
    for function in (current, context, processing):
        function.argtypes = []; function.restype = c.c_void_p
    main_slots = (current(), context(), processing())
    child = []
    def thread_probe():
        old = setter(callback)
        try:
            child.append((current(), context(), processing(), getter() == callback))
        finally:
            setter(old)
    worker = threading.Thread(target=thread_probe); worker.start(); worker.join()
    check(child and child[0][3] and getter() == callback and all(a != b for a, b in zip(main_slots, child[0][:3])), "EH slots are thread-local")
finally:
    setter(previous)
check(getter() == previous, "Native terminate handler restored")
check(hasattr(vcrt, "__CxxFrameHandler3") and hasattr(fh4, "__CxxFrameHandler4"), "Both native frame metadata formats available")
ntdll = c.WinDLL("ntdll.dll")
image_base = ntdll.RtlPcToFileHeader
image_base.argtypes = [c.c_void_p, c.POINTER(c.c_void_p)]; image_base.restype = c.c_void_p
base = c.c_void_p()
heap_buffer = c.create_string_buffer(64)
check(not image_base(c.addressof(heap_buffer), c.byref(base)), "Heap allocation is not a PE image")
address = c.cast(vcrt.__CxxFrameHandler3, c.c_void_p).value
check(image_base(address, c.byref(base)) == vcrt._handle and base.value == vcrt._handle, "Native metadata/function image lookup")
print(f"Passed {passed} native MSVC exception/RTTI contracts; bridge C++ adapters and throw/catch fixtures were not built or executed.")
