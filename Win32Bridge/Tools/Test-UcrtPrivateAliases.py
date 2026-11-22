"""Probe native UCRT private wrapper ABI; never builds or executes bridge C++."""
import ctypes as c
import sys

if sys.platform != 'win32':
    raise SystemExit('Windows only')
ucrt = c.CDLL('ucrtbase')
checks = 0

def bind(name, result, *args):
    fn = getattr(ucrt, name)
    fn.restype, fn.argtypes = result, list(args)
    return fn

def check(value):
    global checks
    assert value
    checks += 1

for public in ['_wcsicmp', '_wcsnicmp']:
    args = (c.c_wchar_p, c.c_wchar_p) + ((c.c_size_t,) if public.endswith('nicmp') else ())
    normal, private = [bind(prefix + public, c.c_int, *args) for prefix in ('', '_o_')]
    values = ('Hello', 'hello') + ((5,) if public.endswith('nicmp') else ())
    check(normal(*values) == private(*values) == 0)
for public in ['towlower', 'towupper', 'iswalpha', 'iswdigit', 'iswspace', 'iswxdigit']:
    normal, private = [bind(prefix + public, c.c_uint, c.c_ushort) for prefix in ('', '_o_')]
    for ch in ['A', '9', ' ']:
        check(normal(ord(ch)) == private(ord(ch)))
normal = bind('wcstoul', c.c_ulong, c.c_wchar_p, c.c_void_p, c.c_int)
private = bind('_o_wcstoul', c.c_ulong, c.c_wchar_p, c.c_void_p, c.c_int)
check(normal('ff', None, 16) == private('ff', None, 16) == 255)
for prefix in ('', '_o_'):
    malloc, free = bind(prefix+'malloc', c.c_void_p, c.c_size_t), bind(prefix+'free', None, c.c_void_p)
    pointer = malloc(32)
    check(bool(pointer))
    free(pointer)
srand, rand = bind('srand', None, c.c_uint), bind('rand', c.c_int)
osrand, orand = bind('_o_srand', None, c.c_uint), bind('_o_rand', c.c_int)
srand(17); expected = [rand() for _ in range(4)]
osrand(17); check([orand() for _ in range(4)] == expected)
pipe = bind('_pipe', c.c_int, c.POINTER(c.c_int), c.c_uint, c.c_int)
read = bind('_read', c.c_int, c.c_int, c.c_void_p, c.c_uint)
write = bind('_write', c.c_int, c.c_int, c.c_void_p, c.c_uint)
close = bind('_close', c.c_int, c.c_int)
fds = (c.c_int * 2)()
check(pipe(fds, 128, 0x8000 | 0x80) == 0)  # binary, noninheritable
try:
    check(write(fds[1], b'pipe', 4) == 4)
    buffer = c.create_string_buffer(4)
    check(read(fds[0], buffer, 4) == 4 and buffer.raw == b'pipe')
finally:
    close(fds[0]); close(fds[1])
print(f'PASS: {checks} native UCRT wrapper/pipe reference checks; bridge C++ not compiled or executed.')
