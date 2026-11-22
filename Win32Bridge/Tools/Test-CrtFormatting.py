"""Probe native UCRT formatting contracts without building Win32Bridge.

This verifies backend recipes and ABI assumptions, not the bridge C++ adapters.
Run on Windows x64 with Python/ctypes; no files or process-wide CRT state change.
"""
import ctypes as c
import struct

if c.sizeof(c.c_void_p) != 8 or c.sizeof(c.c_wchar) != 2:
    raise SystemExit("Windows x64 ctypes ABI required")

crt = c.CDLL("ucrtbase.dll")
handler_type = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p, c.c_void_p, c.c_uint, c.c_size_t)
validation_events = []

@handler_type
def validation_handler(expression, function, file, line, reserved):
    validation_events.append(line)

set_handler = crt._set_thread_local_invalid_parameter_handler
set_handler.argtypes = [c.c_void_p]
set_handler.restype = c.c_void_p
get_errno = crt._get_errno
get_errno.argtypes = [c.POINTER(c.c_int)]
get_errno.restype = c.c_int


def invoke(name, options, buffer, capacity, fmt, values=(), maximum=None):
    wide = "vswprintf" in name or "vsnwprintf" in name
    text = c.create_unicode_buffer(fmt) if wide else c.create_string_buffer(fmt)
    keepalive = [text]
    slots = []
    for value in values:
        if isinstance(value, bytes):
            allocation = c.create_string_buffer(value)
            keepalive.append(allocation)
            slots.append(c.addressof(allocation))
        elif isinstance(value, str):
            allocation = c.create_unicode_buffer(value)
            keepalive.append(allocation)
            slots.append(c.addressof(allocation))
        elif isinstance(value, float):
            slots.append(struct.unpack("<Q", struct.pack("<d", value))[0])
        else:
            slots.append(int(value) & ((1 << 64) - 1))
    arguments = (c.c_uint64 * max(1, len(slots)))(*slots)
    function = getattr(crt, name)
    common = [c.c_uint64, c.c_void_p, c.c_size_t]
    if maximum is not None:
        common.append(c.c_size_t)
    function.argtypes = common + [c.c_void_p, c.c_void_p, c.c_void_p]
    function.restype = c.c_int
    params = [options, c.addressof(buffer) if buffer is not None else None, capacity]
    if maximum is not None:
        params.append(maximum)
    result = function(*params, c.addressof(text), None, c.addressof(arguments))
    error = c.c_int()
    get_errno(c.byref(error))
    return result, error.value


passed = 0

def check(condition, label):
    global passed
    if not condition:
        raise AssertionError(label)
    passed += 1


previous = set_handler(c.cast(validation_handler, c.c_void_p))
try:
    buffer = c.create_string_buffer(b"X" * 129, 130)
    result, _ = invoke("__stdio_common_vsprintf", 2, buffer, 3, b"%s", [b"abcd"])
    check(result == 4 and buffer.raw[:4] == b"ab\0X", "C99 required-size/truncation")
    buffer.raw = b"X" * 130
    result, _ = invoke("__stdio_common_vsprintf", 1, buffer, 3, b"%s", [b"abc"])
    check(result == 3 and buffer.raw[:4] == b"abcX", "Legacy exact-fit/non-termination")
    result, _ = invoke("__stdio_common_vsprintf", 0, buffer, 3, b"%s", [b"abc"])
    check(result == -2 and buffer.raw[:3] == b"ab\0", "Terminated bounded truncation")
    result, _ = invoke("__stdio_common_vsprintf", 28, buffer, 128, b"%I64d", [-1234567890123456])
    check(result == 17 and buffer.value == b"-1234567890123456", "Legacy I64 integer")
    result, _ = invoke("__stdio_common_vsprintf", 28, buffer, 128, b"%.1e", [1.0])
    check(result > 0 and b"e+000" in buffer.value, "Legacy three-digit exponent")
    result, _ = invoke("__stdio_common_vsprintf_p", 0, buffer, 128, b"%2$d:%1$s", [b"name", 7])
    check(result == 6 and buffer.value == b"7:name", "Positional formatting")
    wide = c.create_unicode_buffer(128)
    result, _ = invoke("__stdio_common_vswprintf", 0, wide, 128, "%s/%ls", [b"n", "w"])
    check(result == 3 and wide.value == "n/w", "ISO wide specifiers")
    result, _ = invoke("__stdio_common_vswprintf", 4, wide, 128, "%s/%hs", ["w", b"n"])
    check(result == 3 and wide.value == "w/n", "Legacy wide specifiers")
    result, _ = invoke("__stdio_common_vsprintf_s", 0, buffer, 3, b"%s", [b"abcd"])
    check(result == -1 and not buffer.raw[0] and bool(validation_events), "Secure truncation/returning handler")
    result, _ = invoke("__stdio_common_vsnprintf_s", 0, buffer, 3, b"%s", [b"abcd"], maximum=(1 << 64) - 1)
    check(result == -1 and buffer.value == b"ab", "Secure _TRUNCATE")
    result, _ = invoke("__stdio_common_vsprintf", 2, None, 0, b"%d:%d", [10, 20])
    check(result == 5, "Length measurement")
    result, _ = invoke("__stdio_common_vsprintf", 2, buffer, 6, b"%d:%d", [10, 20])
    check(result == 5 and buffer.value == b"10:20", "Measured render")
    result, _ = invoke("__stdio_common_vswprintf", 2, wide, 128, "A%cB", [0])
    check(result == 3 and wide[:4] == "A\0B\0", "Embedded wide NUL")
finally:
    set_handler(previous)

print(f"Passed {passed} native UCRT contract checks; bridge C++ adapters were not built or executed.")