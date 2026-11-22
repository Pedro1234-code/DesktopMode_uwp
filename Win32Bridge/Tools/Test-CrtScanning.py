"""Native UCRT scanf contract probes; does not build or execute the bridge."""
import ctypes as c
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

def scan(options, text, count, fmt, arguments):
    wide = isinstance(text, str)
    input_buffer = c.create_unicode_buffer(text) if wide else c.create_string_buffer(text)
    format_buffer = c.create_unicode_buffer(fmt) if wide else c.create_string_buffer(fmt)
    slots = (c.c_uint64 * max(1, len(arguments)))(*arguments)
    function = crt.__stdio_common_vswscanf if wide else crt.__stdio_common_vsscanf
    function.argtypes = [c.c_uint64, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p, c.c_void_p]
    function.restype = c.c_int
    result = function(options, c.addressof(input_buffer), count, c.addressof(format_buffer), None, c.addressof(slots))
    error = c.c_int(); get_errno(c.byref(error))
    return result, error.value


def scan_file(options, text, fmt, arguments):
    """Probe an owned native temporary FILE; never receives a guest handle."""
    crt.tmpfile.argtypes = []; crt.tmpfile.restype = c.c_void_p
    crt.fclose.argtypes = [c.c_void_p]; crt.fclose.restype = c.c_int
    crt.fwrite.argtypes = [c.c_void_p, c.c_size_t, c.c_size_t, c.c_void_p]; crt.fwrite.restype = c.c_size_t
    crt.rewind.argtypes = [c.c_void_p]; crt.rewind.restype = None
    crt.ftell.argtypes = [c.c_void_p]; crt.ftell.restype = c.c_long
    crt.fgetc.argtypes = [c.c_void_p]; crt.fgetc.restype = c.c_int
    stream = crt.tmpfile()
    if not stream:
        raise RuntimeError("Native tmpfile unavailable")
    try:
        data = c.create_string_buffer(text)
        if crt.fwrite(data, 1, len(text), stream) != len(text):
            raise RuntimeError("Native fixture write failed")
        crt.rewind(stream)
        wide = isinstance(fmt, str)
        format_buffer = c.create_unicode_buffer(fmt) if wide else c.create_string_buffer(fmt)
        slots = (c.c_uint64 * max(1, len(arguments)))(*arguments)
        function = crt.__stdio_common_vfwscanf if wide else crt.__stdio_common_vfscanf
        function.argtypes = [c.c_uint64, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p]
        function.restype = c.c_int
        crt._set_errno(0)
        result = function(options, stream, c.addressof(format_buffer), None, c.addressof(slots))
        error = c.c_int(); get_errno(c.byref(error))
        position = crt.ftell(stream)
        following = crt.fgetc(stream)
        return result, error.value, position, following
    finally:
        crt.fclose(stream)
passed = 0
def check(condition, label):
    global passed
    if not condition:
        raise AssertionError(label)
    passed += 1

previous = set_handler(c.cast(validation_handler, c.c_void_p))
try:
    unlimited = (1 << 64) - 1
    number, negative, position = c.c_int(), c.c_int(), c.c_int(-1)
    real = c.c_double()
    result, _ = scan(0, b"0x2a -17 3.5 rest", unlimited, b"%i %d %lf %n", [c.addressof(number), c.addressof(negative), c.addressof(real), c.addressof(position)])
    check(result == 3 and number.value == 42 and negative.value == -17 and real.value == 3.5 and position.value == 13, "Numbers and %n")
    text = c.create_string_buffer(8)
    result, _ = scan(0, b"12 abc", unlimited, b"%*d %2s%n", [c.addressof(text), c.addressof(position)])
    check(result == 1 and text.value == b"ab" and position.value == 5, "Suppression and field width")
    character = c.c_char()
    result, _ = scan(0, b"123-Z", unlimited, b"%3[0-9]-%c", [c.addressof(text), c.addressof(character)])
    check(result == 2 and text.value == b"123" and character.value == b"Z", "Scanset and literal")
    result, _ = scan(0, b"1234", 2, b"%d%n", [c.addressof(number), c.addressof(position)])
    check(result == 1 and number.value == 12 and position.value == 2, "Counted input")
    number.value = 777
    result, _ = scan(0, b"x", unlimited, b"%d", [c.addressof(number)])
    check(result == 0 and number.value == 777, "Matching failure")
    result, _ = scan(0, b"", unlimited, b"%d", [c.addressof(number)])
    check(result == -1 and number.value == 777, "Input failure")
    result, _ = scan(1, b"abc", unlimited, b"%3s%n", [c.addressof(text), 4, c.addressof(position)])
    check(result == 1 and text.value == b"abc" and position.value == 3, "Secure string and %n")
    result, error = scan(1, b"abc", unlimited, b"%s", [c.addressof(text), 3])
    check(result <= 0 and not text.raw[0] and error == 12, "Secure insufficient buffer")
    wide_text = c.create_unicode_buffer(16)
    result, _ = scan(0, "texto", unlimited, "%ls%n", [c.addressof(wide_text), c.addressof(position)])
    check(result == 1 and wide_text.value == "texto" and position.value == 5, "Wide explicit target")
    result, _ = scan(2, "wide", unlimited, "%s", [c.addressof(wide_text)])
    check(result == 1 and wide_text.value == "wide", "Legacy wide target")
    result, _ = scan(0, "narrow", unlimited, "%s", [c.addressof(text)])
    check(result == 1 and text.value == b"narrow", "ISO wide input/narrow target")
    raw = c.create_string_buffer(b"XXX", 3)
    result, _ = scan(0, b"ab", unlimited, b"%3c", [c.addressof(raw)])
    check(result == -1 and raw.raw[:2] == b"ab", "Strict character field width")
    result, _ = scan(4, b"ab", unlimited, b"%3c", [c.addressof(raw)])
    check(result == 1 and raw.raw[:2] == b"ab", "Legacy partial character field")
    result, _ = scan(0, b"", unlimited, b"%n", [c.addressof(position)])
    check(result == 0 and position.value == 0, "Zero-input %n")
    result, _, cursor, following = scan_file(0, b"42 tail", b"%d%n", [c.addressof(number), c.addressof(position)])
    check(result == 1 and number.value == 42 and position.value == 2 and cursor == 2 and following == 32, "Stream delimiter retained")
    for malformed, expected_cursor, expected_following in [(b"+x", 1, ord('x')), (b"0xZ", 2, ord('Z')), (b"1e+X", 3, ord('X')), (b".e2", 1, ord('e')), (b"infiZ", 4, ord('Z'))]:
        fmt = b"%i" if malformed in (b"+x", b"0xZ") else b"%lf"
        target = c.addressof(number) if fmt == b"%i" else c.addressof(real)
        result, _, cursor, following = scan_file(0, malformed, fmt, [target])
        check(result == 0 and cursor == expected_cursor and following == expected_following, f"Malformed subject consumption: {malformed!r}")
    result, _, cursor, following = scan_file(0, b"ab\0cZ", b"%4c%n", [c.addressof(text), c.addressof(position)])
    check(result == 1 and text.raw[:4] == b"ab\0c" and position.value == 4 and cursor == 4 and following == ord('Z'), "Stream embedded NUL")
    result, error, cursor, following = scan_file(1, b"abcd!", b"%s", [c.addressof(text), 3])
    check(result == 0 and error == 12 and not text.raw[0] and cursor == 3 and following == ord('d'), "Secure stream overflow consumes excess character only")
    result, error, cursor, following = scan_file(1, b"abc", b"%s", [c.addressof(text), 0])
    check(result == 0 and error == 12 and cursor == 0 and following == ord('a'), "Secure zero-capacity stream")
    result, error, cursor, following = scan_file(5, b"abc", b"%s", [c.addressof(text), 0])
    check(result == 0 and error == 12 and cursor == 1 and following == ord('b') and not text.raw[0], "Legacy zero-capacity stream")
    text[3] = b'Q'
    result, error, cursor, following = scan_file(1, b"abc", b"%s", [c.addressof(text), 3])
    check((result, error, cursor, following) == (-1, 12, 3, -1) and text.raw[0] == 0 and text.raw[3] == ord('Q'), "Excess last byte: EOF, cleared string, untouched guard")
    for options, expected_error in ((0, 0), (1, 22)):
        result, error, cursor, following = scan_file(options, b"12 X", b"%d%q", [c.addressof(number)])
        check((result, error, cursor, following) == (1, expected_error, 2, ord(' ')) and number.value == 12, "Invalid format after assignment")
    result, error, cursor, following = scan_file(0, b"abc X", b"%18446744073709551616s%n", [c.addressof(text), c.addressof(position)])
    check((result, error, cursor, following) == (1, 34, 3, ord(' ')) and text.value == b"abc" and position.value == 3, "Saturating field width")
    result, error, cursor, following = scan_file(0, "123 X".encode("utf-16le"), "%\u0662d%n", [c.addressof(number), c.addressof(position)])
    check((result, error, cursor, following) == (1, 0, 4, ord('3')) and number.value == 12 and position.value == 2, "Unicode decimal field width")
    result, error, cursor, following = scan_file(0, "\u3000A\u3000B X".encode("utf-16le"), "%ls%n", [c.addressof(wide_text), c.addressof(position)])
    check((result, error, cursor, following) == (1, 0, 8, ord(' ')) and wide_text.value == "A\u3000B" and position.value == 4, "Locale whitespace at start only")
    crt._configthreadlocale.argtypes = [c.c_int]
    crt._wsetlocale.argtypes = [c.c_int, c.c_wchar_p]
    crt._wsetlocale.restype = c.c_wchar_p
    old_mode = crt._configthreadlocale(1)
    old_locale = crt._wsetlocale(2, None)
    try:
        check(bool(crt._wsetlocale(2, "Japanese_Japan.932")), "Select CP932 reference locale")
        result, error, cursor, following = scan_file(1, b"\x82\xa0X", b"\x82", [])
        check((result, error, cursor, following) == (0, 42, 0, 0x82), "Malformed DBCS literal is rejected before input")
        result, error, cursor, following = scan_file(0, b"\x82BX", b"\x82\xa0", [])
        check((result, error, cursor, following) == (0, 0, 0, 0x82), "DBCS mismatch leaves the entire literal unread")
    finally:
        crt._wsetlocale(2, old_locale)
        crt._configthreadlocale(old_mode)
    result, _, cursor, following = scan_file(0, b"abcd!", b"%[a-c-e]", [c.addressof(text)])
    check(result == 1 and text.value == b"abc" and cursor == 3 and following == ord('d'), "Scanset range endpoints")
    result, _, cursor, following = scan_file(0, b"abc!", b"%[c-a]", [c.addressof(text)])
    check(result == 1 and text.value == b"abc" and cursor == 3 and following == ord('!'), "Descending scanset range")
    result, _, cursor, following = scan_file(0, b"12\0X", b"%d%n", [c.addressof(number), c.addressof(position)])
    check(result == 1 and position.value == 3 and cursor == 3 and following == ord('X'), "Numeric NUL sentinel consumption")
finally:
    set_handler(previous)
print(f"Passed {passed} native UCRT scanf contract checks; bridge C++ adapters were not built or executed.")