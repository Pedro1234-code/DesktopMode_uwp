"""Native Windows/UCRT reference contracts; never loads or builds the bridge."""
import ctypes as c
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from ctypes import wintypes as w


def contracts():
    k = c.WinDLL("kernel32", use_last_error=True)
    crt = c.CDLL("ucrtbase", use_errno=True)
    def bind(lib, name, result, *args):
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, list(args)
        return fn
    create = bind(k, "CreatePipe", w.BOOL, c.POINTER(w.HANDLE), c.POINTER(w.HANDLE), c.c_void_p, w.DWORD)
    getstd = bind(k, "GetStdHandle", w.HANDLE, w.DWORD)
    peek = bind(k, "PeekNamedPipe", w.BOOL, w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.POINTER(w.DWORD), c.POINTER(w.DWORD))
    adopt = bind(crt, "_open_osfhandle", c.c_int, c.c_ssize_t, c.c_int)
    handle = bind(crt, "_get_osfhandle", c.c_ssize_t, c.c_int)
    close = bind(crt, "_close", c.c_int, c.c_int)
    dup = bind(crt, "_dup", c.c_int, c.c_int)
    dup2 = bind(crt, "_dup2", c.c_int, c.c_int, c.c_int)
    read = bind(crt, "_read", c.c_int, c.c_int, c.c_void_p, c.c_uint)
    write = bind(crt, "_write", c.c_int, c.c_int, c.c_void_p, c.c_uint)
    terminal = bind(crt, "_isatty", c.c_int, c.c_int)
    iob = bind(crt, "__acrt_iob_func", c.c_void_p, c.c_uint)
    fwrite = bind(crt, "fwrite", c.c_size_t, c.c_void_p, c.c_size_t, c.c_size_t, c.c_void_p)
    fread = bind(crt, "fread", c.c_size_t, c.c_void_p, c.c_size_t, c.c_size_t, c.c_void_p)
    fflush = bind(crt, "fflush", c.c_int, c.c_void_p)
    fdopen = bind(crt, "_fdopen", c.c_void_p, c.c_int, c.c_char_p)
    fclose = bind(crt, "fclose", c.c_int, c.c_void_p)
    feof = bind(crt, "feof", c.c_int, c.c_void_p)
    ferror = bind(crt, "ferror", c.c_int, c.c_void_p)
    checks = 0
    def check(value, label):
        nonlocal checks
        if not value:
            raise AssertionError(label)
        checks += 1
    def pipe(text=True):
        r, t = w.HANDLE(), w.HANDLE()
        check(create(c.byref(r), c.byref(t), None, 128), "CreatePipe")
        mode = 0x4000 if text else 0x8000
        a, b = adopt(r.value, mode), adopt(t.value, mode | 1)
        check(a >= 0 and b >= 0, "adopt non-seekable endpoints")
        return a, b

    r, t = pipe()
    check(not terminal(r) and not terminal(t), "pipe is not terminal")
    check(write(t, b"a\rX\nb", 5) == 5, "text write logical byte count")
    check(close(t) == 0, "close writer")
    data = bytearray()
    buf = c.create_string_buffer(32)
    while True:
        n = read(r, buf, 1)
        check(n >= 0, "text read succeeds")
        if not n:
            break
        data += buf.raw[:n]
    check(data == b"a\rX\nb", "CR lookahead retained without seeking")
    close(r)

    r, t = pipe()
    twin = dup(t)
    check(twin >= 0 and handle(twin) != handle(t), "dup creates distinct OS handle")
    check(close(t) == 0 and write(twin, b"ok", 2) == 2, "duplicate survives source close")
    check(close(twin) == 0 and read(r, buf, 32) == 2 and buf.raw[:2] == b"ok", "drain before EOF")
    check(read(r, buf, 32) == 0, "final writer EOF")
    close(r)

    r, t = pipe()
    check(write(t, b"a\x1ab", 3) == 3 and close(t) == 0, "Ctrl-Z payload")
    check(read(r, buf, 32) == 1 and buf.raw[:1] == b"a" and read(r, buf, 32) == 0, "Ctrl-Z text EOF")
    close(r)

    saved = dup(1)
    check(saved >= 0, "save stdout in isolated process")
    r, t = pipe()
    try:
        check(dup2(t, 1) == 0 and getstd(0xfffffff5) == handle(1), "dup2 synchronizes Win32 stdout")
        check(handle(1) != handle(t) and close(t) == 0, "stdout owns duplicate handle")
        stdout = iob(1)
        check(fwrite(b"stdio\n", 1, 6, stdout) == 6 and fflush(stdout) == 0, "FILE output uses redirected descriptor")
        physical, copied = c.create_string_buffer(32), w.DWORD()
        check(peek(handle(r), physical, 32, c.byref(copied), None, None) and physical.raw[:copied.value] == b"stdio\r\n", "physical CRLF bytes")
        check(close(1) == 0 and not getstd(0xfffffff5), "close clears Win32 stdout")
        stream = fdopen(r, b"rt")
        check(bool(stream), "FILE input on pipe")
        check(fread(buf, 1, 32, stream) == 6 and buf.raw[:6] == b"stdio\n", "FILE input translates CRLF")
        check(feof(stream) and not ferror(stream), "pipe EOF is not FILE error")
        fclose(stream)
    finally:
        dup2(saved, 1)
        close(saved)
    setvbuf = bind(crt, "setvbuf", c.c_int, c.c_void_p, c.c_void_p, c.c_int, c.c_size_t)
    fputc = bind(crt, "fputc", c.c_int, c.c_int, c.c_void_p)
    for size in (6, 7):
        r, t = pipe(text=False)
        check(close(r) == 0, "Close buffered pipe reader")
        stream = fdopen(t, b"wb")
        check(bool(stream), "Open buffered pipe FILE")
        storage = c.create_string_buffer(8)
        try:
            check(setvbuf(stream, storage, 0, len(storage)) == 0, "Install FILE output buffer")
            check(fwrite(b"ab", 1, 2, stream) == 2 and not ferror(stream), "Queue initial output")
            check(fwrite(b"1234567", 1, size, stream) == 6, "Only bytes copied before failed flush count")
            if size == 6:
                check(not ferror(stream) and storage.raw == b"ab123456", "Filling the buffer does not flush")
                check(fputc(ord('Z'), stream) == -1 and ferror(stream), "Next character exposes failed flush")
            else:
                check(bool(ferror(stream)), "Oversized write reports failed flush")
        finally:
            fclose(stream)

    setmode = bind(crt, "_setmode", c.c_int, c.c_int, c.c_int)
    fgetwc = bind(crt, "fgetwc", c.c_ushort, c.c_void_p)
    fgetc = bind(crt, "fgetc", c.c_int, c.c_void_p)
    fputws = bind(crt, "fputws", c.c_int, c.c_wchar_p, c.c_void_p)
    wfopen = bind(crt, "_wfopen_s", c.c_int, c.POINTER(c.c_void_p), c.c_wchar_p, c.c_wchar_p)
    setlocale = bind(crt, "_wsetlocale", c.c_wchar_p, c.c_int, c.c_wchar_p)
    r, t = pipe(text=False)
    utf8 = b"\xf0\x9f\x99\x82A\r\n"
    check(write(t, utf8, len(utf8)) == len(utf8) and close(t) == 0, "Write supplementary UTF-8 into pipe")
    check(setmode(r, 0x40000) != -1, "Set _O_U8TEXT on pipe read end")
    stream = fdopen(r, b"r")
    check(bool(stream), "Open wide FILE over pipe")
    try:
        check([fgetwc(stream) for _ in range(4)] == [0xd83d, 0xde42, ord('A'), ord('\n')], "Wide pipe preserves surrogate pair and following input")
        check(fgetwc(stream) == 0xffff and feof(stream) and not ferror(stream), "Wide pipe EOF")
    finally:
        fclose(stream)

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "wide.txt"
        def open_file(mode):
            stream = c.c_void_p()
            assert wfopen(c.byref(stream), str(path), mode) == 0 and stream.value
            return stream
        for locale, raw, expected in (("C", b"\xe9A", [0xe9, ord('A')]),
                                      ("Japanese_Japan.932", b"\x82\xa0A", [0x3042, ord('A')]),
                                      (".UTF-8", b"\xc3\xa9A", [0xe9, ord('A')])):
            assert setlocale(2, locale)
            path.write_bytes(raw)
            stream = open_file("rt")
            try:
                check([fgetwc(stream), fgetwc(stream)] == expected, f"ANSI mode LC_CTYPE read: {locale}")
            finally:
                fclose(stream)
        assert setlocale(2, "C")
        stream = open_file("wt")
        try:
            c.set_errno(0)
            check(fputws("A\u3042Z", stream) == -1 and c.get_errno() == 42 and not ferror(stream), "Unrepresentable C locale output is EILSEQ")
        finally:
            fclose(stream)
        check(path.read_bytes() == b"A", "Wide string preserves the successfully written prefix")
        assert setlocale(2, "Japanese_Japan.932")
        stream = open_file("wt")
        try:
            check(fputws("\u3042\n", stream) == 0, "Write DBCS wide string")
        finally:
            fclose(stream)
        check(path.read_bytes() == b"\x82\xa0\r\n", "DBCS wide output and CRLF translation")
        path.write_bytes(b"\x82")
        stream = open_file("rt")
        try:
            check(fgetwc(stream) == 0xffff and not feof(stream) and not ferror(stream) and fgetc(stream) == 0x82,
                  "Truncated DBCS lead is available to next byte read")
        finally:
            fclose(stream)
    return checks


if __name__ == "__main__":
    if sys.platform != "win32":
        raise SystemExit("Windows reference probe only")
    if "--child" in sys.argv:
        print(json.dumps({"checks": contracts()}), file=sys.stderr)
    else:
        result = subprocess.run([sys.executable, __file__, "--child"], capture_output=True, text=True, check=True)
        count = json.loads(result.stderr)["checks"]
        print(f"PASS: {count} native UCRT/Windows stream contracts. Bridge C++ was not built or executed.")
