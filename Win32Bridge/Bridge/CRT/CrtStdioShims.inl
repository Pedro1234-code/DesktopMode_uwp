// Included after the process and lowio CRT adapters.
namespace
{
    Win32Bridge::Bridge::CrtStdio* CurrentCrtStdio()
    {
        auto loader = Win32Bridge::Bridge::CurrentGuestModuleLoader();
        if (!loader) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return nullptr; }
        return &loader->Stdio();
    }
    FILE* OpenCrtStdio(const wchar_t* path, const wchar_t* mode, int sharing)
    {
        auto io = CurrentCrtStdio(); auto state = CurrentCrt();
        if (!io || !state) return nullptr;
        int defaultMode = _O_TEXT; BridgeGetFileMode(&defaultMode);
        const int mask = static_cast<int>(::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&state->fileCreationMask), 0, 0));
        return static_cast<FILE*>(io->Open(path, mode, sharing, defaultMode, mask, state->commitMode != 0));
    }
    FILE* __cdecl BridgeStdioWFOpen(const wchar_t* path, const wchar_t* mode) { return OpenCrtStdio(path, mode, _SH_DENYNO); }
    FILE* __cdecl BridgeStdioFOpen(const char* path, const char* mode)
    {
        std::wstring widePath, wideMode;
        if (!CrtFileWidePath(path, &widePath) || !CrtFileWidePath(mode, &wideMode)) return nullptr;
        return OpenCrtStdio(widePath.c_str(), wideMode.c_str(), _SH_DENYNO);
    }
    FILE* __cdecl BridgeStdioWFSOpen(const wchar_t* path, const wchar_t* mode, int share) { return OpenCrtStdio(path, mode, share); }
    FILE* __cdecl BridgeStdioFSOpen(const char* path, const char* mode, int share)
    {
        std::wstring widePath, wideMode;
        if (!CrtFileWidePath(path, &widePath) || !CrtFileWidePath(mode, &wideMode)) return nullptr;
        return OpenCrtStdio(widePath.c_str(), wideMode.c_str(), share);
    }
    int __cdecl BridgeStdioWFOpenS(FILE** output, const wchar_t* path, const wchar_t* mode)
    {
        if (!output) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        *output = OpenCrtStdio(path, mode, _SH_SECURE); return *output ? 0 : errno;
    }
    int __cdecl BridgeStdioFOpenS(FILE** output, const char* path, const char* mode)
    {
        if (!output) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        *output = nullptr; std::wstring widePath, wideMode;
        if (!CrtFileWidePath(path, &widePath) || !CrtFileWidePath(mode, &wideMode)) return errno;
        return BridgeStdioWFOpenS(output, widePath.c_str(), wideMode.c_str());
    }
    FILE* __cdecl BridgeStdioWFDOpen(int descriptor, const wchar_t* mode)
    {
        auto io = CurrentCrtStdio(); auto state = CurrentCrt();
        return io && state ? static_cast<FILE*>(io->Attach(descriptor, mode, state->commitMode != 0)) : nullptr;
    }
    FILE* __cdecl BridgeStdioFDOpen(int descriptor, const char* mode)
    { std::wstring wideMode; return CrtFileWidePath(mode, &wideMode) ? BridgeStdioWFDOpen(descriptor, wideMode.c_str()) : nullptr; }
    FILE* __cdecl BridgeStdioWReopen(const wchar_t* path, const wchar_t* mode, FILE* stream)
    {
        auto io = CurrentCrtStdio(); auto state = CurrentCrt(); if (!io || !state) return nullptr;
        int defaultMode = _O_TEXT; BridgeGetFileMode(&defaultMode);
        const int mask = static_cast<int>(::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&state->fileCreationMask), 0, 0));
        return static_cast<FILE*>(io->Reopen(path, mode, stream, defaultMode, mask, state->commitMode != 0));
    }
    FILE* __cdecl BridgeStdioReopen(const char* path, const char* mode, FILE* stream)
    {
        std::wstring widePath, wideMode;
        return CrtFileWidePath(path, &widePath) && CrtFileWidePath(mode, &wideMode) ? BridgeStdioWReopen(widePath.c_str(), wideMode.c_str(), stream) : nullptr;
    }
    int __cdecl BridgeStdioWReopenS(FILE** output, const wchar_t* path, const wchar_t* mode, FILE* stream)
    {
        if (!output) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        *output = BridgeStdioWReopen(path, mode, stream); return *output ? 0 : errno;
    }
    int __cdecl BridgeStdioReopenS(FILE** output, const char* path, const char* mode, FILE* stream)
    {
        if (!output) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        *output = BridgeStdioReopen(path, mode, stream); return *output ? 0 : errno;
    }
    int __cdecl BridgeStdioFClose(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Close(stream) : -1; }
    int __cdecl BridgeStdioFCloseAll() { auto io = CurrentCrtStdio(); return io ? io->CloseAll() : -1; }
    int __cdecl BridgeStdioFlush(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Flush(stream) : -1; }
    int __cdecl BridgeStdioFlushAll() { auto io = CurrentCrtStdio(); return io ? io->FlushAll(true) : -1; }
    size_t __cdecl BridgeStdioRead(void* buffer, size_t size, size_t count, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Read(buffer, size, count, stream) : 0; }
    size_t __cdecl BridgeStdioReadS(void* buffer, size_t capacity, size_t size, size_t count, FILE* stream)
    {
        if (!size || !count) return 0;
        if (!buffer || count > (std::numeric_limits<size_t>::max)() / size || size * count > capacity)
        {
            if (buffer && capacity != (std::numeric_limits<size_t>::max)()) memset(buffer, 0, capacity);
            Win32Bridge::Bridge::CrtDescriptorFailure(!buffer ? EINVAL : ERANGE); return 0;
        }
        return BridgeStdioRead(buffer, size, count, stream);
    }
    size_t __cdecl BridgeStdioWrite(const void* buffer, size_t size, size_t count, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Write(buffer, size, count, stream) : 0; }
    int __cdecl BridgeStdioGetByte(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->GetByte(stream) : -1; }
    int __cdecl BridgeStdioPutByte(int character, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->PutByte(character, stream) : -1; }
    int __cdecl BridgeStdioUngetByte(int character, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->UngetByte(character, stream) : -1; }
    char* __cdecl BridgeStdioGetLine(char* buffer, int count, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->GetLine(buffer, count, stream) : nullptr; }
    int __cdecl BridgeStdioPutString(const char* text, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->PutString(text, stream) : -1; }
    wint_t __cdecl BridgeStdioGetWide(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->GetWide(stream) : WEOF; }
    wint_t __cdecl BridgeStdioPutWide(wint_t character, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->PutWide(character, stream) : WEOF; }
    wint_t __cdecl BridgeStdioUngetWide(wint_t character, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->UngetWide(character, stream) : WEOF; }
    wchar_t* __cdecl BridgeStdioGetWideLine(wchar_t* buffer, int count, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->GetWideLine(buffer, count, stream) : nullptr; }
    int __cdecl BridgeStdioPutWideString(const wchar_t* text, FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->PutWideString(text, stream) : -1; }
    int __cdecl BridgeStdioEof(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Eof(stream) : 0; }
    int __cdecl BridgeStdioError(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Error(stream) : 0; }
    void __cdecl BridgeStdioClearError(FILE* stream) { if (auto io = CurrentCrtStdio()) io->ClearError(stream); }
    int __cdecl BridgeStdioClearErrorS(FILE* stream)
    { auto io = CurrentCrtStdio(); if (!io || io->Descriptor(stream) < 0) return errno; io->ClearError(stream); return 0; }
    int __cdecl BridgeStdioFileno(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Descriptor(stream) : -1; }
    __int64 __cdecl BridgeStdioTell64(FILE* stream) { auto io = CurrentCrtStdio(); return io ? io->Tell(stream) : -1; }
    long __cdecl BridgeStdioTell(FILE* stream)
    { const auto position = BridgeStdioTell64(stream); return position > LONG_MAX ? Win32Bridge::Bridge::CrtDescriptorFailure(EOVERFLOW) : static_cast<long>(position); }
    int __cdecl BridgeStdioSeek64(FILE* stream, __int64 offset, int origin) { auto io = CurrentCrtStdio(); return io ? io->Seek(stream, offset, origin) : -1; }
    int __cdecl BridgeStdioSeek(FILE* stream, long offset, int origin) { return BridgeStdioSeek64(stream, offset, origin); }
    void __cdecl BridgeStdioRewind(FILE* stream) { if (auto io = CurrentCrtStdio()) io->Rewind(stream); }
    int __cdecl BridgeStdioGetPosition(FILE* stream, __int64* position)
    { if (!position) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); *position = BridgeStdioTell64(stream); return *position < 0 ? -1 : 0; }
    int __cdecl BridgeStdioSetPosition(FILE* stream, const __int64* position)
    { return position ? BridgeStdioSeek64(stream, *position, FILE_BEGIN) : Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); }
    int __cdecl BridgeStdioSetVBuf(FILE* stream, char* buffer, int mode, size_t size) { auto io = CurrentCrtStdio(); return io ? io->SetBuffer(stream, buffer, mode, size) : -1; }
    void __cdecl BridgeStdioSetBuf(FILE* stream, char* buffer) { BridgeStdioSetVBuf(stream, buffer, buffer ? _IOFBF : _IONBF, buffer ? BUFSIZ : 0); }
    void __cdecl BridgeStdioLock(FILE* stream) { if (auto io = CurrentCrtStdio()) io->Lock(stream); }
    void __cdecl BridgeStdioUnlock(FILE* stream) { if (auto io = CurrentCrtStdio()) io->Unlock(stream); }
    int __cdecl BridgeStdioGetMax() { auto io = CurrentCrtStdio(); return io ? io->MaximumStreams() : -1; }
    int __cdecl BridgeStdioSetMax(int count) { auto io = CurrentCrtStdio(); return io ? io->SetMaximumStreams(count) : -1; }
    FILE* __cdecl BridgeStdioIob() { auto io = CurrentCrtStdio(); return io ? reinterpret_cast<FILE*>(io->Iob()) : nullptr; }
    FILE* __cdecl BridgeStdioAcrtIob(unsigned index) { auto io = CurrentCrtStdio(); return io ? static_cast<FILE*>(io->Standard(index)) : nullptr; }
    int __cdecl BridgeStdioGetChar() { return BridgeStdioGetByte(BridgeStdioAcrtIob(0)); }
    int __cdecl BridgeStdioPutChar(int value) { return BridgeStdioPutByte(value, BridgeStdioAcrtIob(1)); }
    wint_t __cdecl BridgeStdioGetWChar() { return BridgeStdioGetWide(BridgeStdioAcrtIob(0)); }
    wint_t __cdecl BridgeStdioPutWChar(wint_t value) { return BridgeStdioPutWide(value, BridgeStdioAcrtIob(1)); }
    int __cdecl BridgeStdioPuts(const char* text)
    { auto io = CurrentCrtStdio(); return io ? io->PutLine(text, io->Standard(1)) : -1; }
    int __cdecl BridgeStdioPutWs(const wchar_t* text)
    { auto io = CurrentCrtStdio(); return io ? io->PutWideLine(text, io->Standard(1)) : -1; }
    int __cdecl BridgeStdioGetWord(FILE* stream) { int word; return BridgeStdioRead(&word, sizeof(word), 1, stream) == 1 ? word : -1; }
    int __cdecl BridgeStdioPutWord(int word, FILE* stream) { return BridgeStdioWrite(&word, sizeof(word), 1, stream) == 1 ? word : -1; }

    Win32Bridge::Bridge::ImportResolution ResolveCrtStdioImport(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
        if (symbol.importedByOrdinal || (!IsMsvcrt(symbol.library) && _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll"))) return result;
#define CRT_STDIO(exportName, function) if (symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&function); else
        CRT_STDIO("fopen", BridgeStdioFOpen)
        CRT_STDIO("_wfopen", BridgeStdioWFOpen)
        CRT_STDIO("fopen_s", BridgeStdioFOpenS)
        CRT_STDIO("_wfopen_s", BridgeStdioWFOpenS)
        CRT_STDIO("_fsopen", BridgeStdioFSOpen)
        CRT_STDIO("_wfsopen", BridgeStdioWFSOpen)
        CRT_STDIO("_fdopen", BridgeStdioFDOpen)
        CRT_STDIO("_wfdopen", BridgeStdioWFDOpen)
        CRT_STDIO("freopen", BridgeStdioReopen)
        CRT_STDIO("_wfreopen", BridgeStdioWReopen)
        CRT_STDIO("freopen_s", BridgeStdioReopenS)
        CRT_STDIO("_wfreopen_s", BridgeStdioWReopenS)
        CRT_STDIO("fclose", BridgeStdioFClose)
        CRT_STDIO("_fclose_nolock", BridgeStdioFClose)
        CRT_STDIO("_fcloseall", BridgeStdioFCloseAll)
        CRT_STDIO("fflush", BridgeStdioFlush)
        CRT_STDIO("_fflush_nolock", BridgeStdioFlush)
        CRT_STDIO("_flushall", BridgeStdioFlushAll)
        CRT_STDIO("fread", BridgeStdioRead)
        CRT_STDIO("fread_s", BridgeStdioReadS)
        CRT_STDIO("_fread_nolock", BridgeStdioRead)
        CRT_STDIO("_fread_nolock_s", BridgeStdioReadS)
        CRT_STDIO("fwrite", BridgeStdioWrite)
        CRT_STDIO("_fwrite_nolock", BridgeStdioWrite)
        CRT_STDIO("fgetc", BridgeStdioGetByte)
        CRT_STDIO("getc", BridgeStdioGetByte)
        CRT_STDIO("_fgetc_nolock", BridgeStdioGetByte)
        CRT_STDIO("_getc_nolock", BridgeStdioGetByte)
        CRT_STDIO("_filbuf", BridgeStdioGetByte)
        CRT_STDIO("fputc", BridgeStdioPutByte)
        CRT_STDIO("putc", BridgeStdioPutByte)
        CRT_STDIO("_fputc_nolock", BridgeStdioPutByte)
        CRT_STDIO("_putc_nolock", BridgeStdioPutByte)
        CRT_STDIO("_flsbuf", BridgeStdioPutByte)
        CRT_STDIO("ungetc", BridgeStdioUngetByte)
        CRT_STDIO("_ungetc_nolock", BridgeStdioUngetByte)
        CRT_STDIO("fgets", BridgeStdioGetLine)
        CRT_STDIO("fputs", BridgeStdioPutString)
        CRT_STDIO("fgetwc", BridgeStdioGetWide)
        CRT_STDIO("getwc", BridgeStdioGetWide)
        CRT_STDIO("_fgetwc_nolock", BridgeStdioGetWide)
        CRT_STDIO("_getwc_nolock", BridgeStdioGetWide)
        CRT_STDIO("fputwc", BridgeStdioPutWide)
        CRT_STDIO("putwc", BridgeStdioPutWide)
        CRT_STDIO("_fputwc_nolock", BridgeStdioPutWide)
        CRT_STDIO("_putwc_nolock", BridgeStdioPutWide)
        CRT_STDIO("ungetwc", BridgeStdioUngetWide)
        CRT_STDIO("_ungetwc_nolock", BridgeStdioUngetWide)
        CRT_STDIO("fgetws", BridgeStdioGetWideLine)
        CRT_STDIO("fputws", BridgeStdioPutWideString)
        CRT_STDIO("feof", BridgeStdioEof)
        CRT_STDIO("ferror", BridgeStdioError)
        CRT_STDIO("clearerr", BridgeStdioClearError)
        CRT_STDIO("clearerr_s", BridgeStdioClearErrorS)
        CRT_STDIO("_fileno", BridgeStdioFileno)
        CRT_STDIO("ftell", BridgeStdioTell)
        CRT_STDIO("_ftell_nolock", BridgeStdioTell)
        CRT_STDIO("_ftelli64", BridgeStdioTell64)
        CRT_STDIO("_ftelli64_nolock", BridgeStdioTell64)
        CRT_STDIO("fseek", BridgeStdioSeek)
        CRT_STDIO("_fseek_nolock", BridgeStdioSeek)
        CRT_STDIO("_fseeki64", BridgeStdioSeek64)
        CRT_STDIO("_fseeki64_nolock", BridgeStdioSeek64)
        CRT_STDIO("rewind", BridgeStdioRewind)
        CRT_STDIO("fgetpos", BridgeStdioGetPosition)
        CRT_STDIO("fsetpos", BridgeStdioSetPosition)
        CRT_STDIO("setbuf", BridgeStdioSetBuf)
        CRT_STDIO("setvbuf", BridgeStdioSetVBuf)
        CRT_STDIO("_lock_file", BridgeStdioLock)
        CRT_STDIO("_unlock_file", BridgeStdioUnlock)
        CRT_STDIO("_getmaxstdio", BridgeStdioGetMax)
        CRT_STDIO("_setmaxstdio", BridgeStdioSetMax)
        CRT_STDIO("__iob_func", BridgeStdioIob)
        CRT_STDIO("__acrt_iob_func", BridgeStdioAcrtIob)
        CRT_STDIO("getchar", BridgeStdioGetChar)
        CRT_STDIO("_fgetchar", BridgeStdioGetChar)
        CRT_STDIO("putchar", BridgeStdioPutChar)
        CRT_STDIO("_fputchar", BridgeStdioPutChar)
        CRT_STDIO("getwchar", BridgeStdioGetWChar)
        CRT_STDIO("_fgetwchar", BridgeStdioGetWChar)
        CRT_STDIO("putwchar", BridgeStdioPutWChar)
        CRT_STDIO("_fputwchar", BridgeStdioPutWChar)
        CRT_STDIO("puts", BridgeStdioPuts)
        CRT_STDIO("_putws", BridgeStdioPutWs)
        CRT_STDIO("_getw", BridgeStdioGetWord)
        CRT_STDIO("_putw", BridgeStdioPutWord)
        { }
#undef CRT_STDIO
        if (symbol.name == L"_iob" && IsMsvcrt(symbol.library))
            if (auto loader = CurrentGuestModuleLoader()) result.targetAddress = reinterpret_cast<ULONGLONG>(loader->Stdio().Iob());
        if (result.targetAddress)
        {
            result.disposition = ImportDisposition::NeedsBridge;
            result.note = L"Process-local FILE streams backed by guest CRT descriptors.";
        }
        return result;
    }
}