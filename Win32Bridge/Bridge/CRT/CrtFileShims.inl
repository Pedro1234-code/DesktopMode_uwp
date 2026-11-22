// Included by MsvcrtShims.cpp, after the process startup adapters.
namespace
{
    Win32Bridge::Bridge::CrtFileDescriptors* CurrentCrtFiles()
    {
        auto loader = Win32Bridge::Bridge::CurrentGuestModuleLoader();
        if (!loader) { Win32Bridge::Bridge::CrtDescriptorFailure(EBADF); return nullptr; }
        return &loader->CrtFiles();
    }
    bool CrtFileWidePath(const char* path, std::wstring* wide)
    {
        if (!path) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return false; }
        const int required = ::MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
        if (!required) { Win32Bridge::Bridge::CrtDescriptorFailure(EILSEQ); return false; }
        try
        {
            wide->resize(static_cast<size_t>(required));
            if (!::MultiByteToWideChar(CP_ACP, 0, path, -1, &(*wide)[0], required))
            { Win32Bridge::Bridge::CrtDescriptorFailure(EILSEQ); return false; }
            wide->resize(static_cast<size_t>(required - 1)); return true;
        }
        catch (const std::bad_alloc&) { Win32Bridge::Bridge::CrtDescriptorFailure(ENOMEM); return false; }
    }
    int OpenCrtFile(const wchar_t* path, int flags, int sharing, int permissions)
    {
        auto files = CurrentCrtFiles(); auto state = CurrentCrt();
        if (!files || !state) return -1;
        int defaultMode = _O_TEXT; BridgeGetFileMode(&defaultMode);
        const int mask = static_cast<int>(::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&state->fileCreationMask), 0, 0));
        try { return files->Open(path, flags, sharing, permissions & ~mask, defaultMode); }
        catch (const std::bad_alloc&) { return Win32Bridge::Bridge::CrtDescriptorFailure(ENOMEM); }
    }
    int __cdecl BridgeWOpen(const wchar_t* path, int flags, ...)
    {
        int permissions = 0600;
        if (flags & _O_CREAT) { va_list args; va_start(args, flags); permissions = va_arg(args, int); va_end(args); }
        return OpenCrtFile(path, flags, _SH_DENYNO, permissions);
    }
    int __cdecl BridgeOpen(const char* path, int flags, ...)
    {
        int permissions = 0600;
        if (flags & _O_CREAT) { va_list args; va_start(args, flags); permissions = va_arg(args, int); va_end(args); }
        std::wstring wide; if (!CrtFileWidePath(path, &wide)) return -1;
        return OpenCrtFile(wide.c_str(), flags, _SH_DENYNO, permissions);
    }
    int __cdecl BridgeWSOpen(const wchar_t* path, int flags, int sharing, ...)
    {
        int permissions = 0600;
        if (flags & _O_CREAT) { va_list args; va_start(args, sharing); permissions = va_arg(args, int); va_end(args); }
        return OpenCrtFile(path, flags, sharing, permissions);
    }
    int __cdecl BridgeSOpen(const char* path, int flags, int sharing, ...)
    {
        int permissions = 0600;
        if (flags & _O_CREAT) { va_list args; va_start(args, sharing); permissions = va_arg(args, int); va_end(args); }
        std::wstring wide; if (!CrtFileWidePath(path, &wide)) return -1;
        return OpenCrtFile(wide.c_str(), flags, sharing, permissions);
    }
    int __cdecl BridgeWSOpenS(int* output, const wchar_t* path, int flags, int sharing, int permissions)
    {
        if (!output) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        *output = OpenCrtFile(path, flags, sharing, permissions); return *output < 0 ? errno : 0;
    }
    int __cdecl BridgeSOpenS(int* output, const char* path, int flags, int sharing, int permissions)
    {
        if (!output) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        *output = -1;
        std::wstring wide; if (!CrtFileWidePath(path, &wide)) return errno;
        return BridgeWSOpenS(output, wide.c_str(), flags, sharing, permissions);
    }
    int __cdecl BridgeWSOpenDispatch(const wchar_t* path, int flags, int sharing, int permissions, int* output, int)
    { return BridgeWSOpenS(output, path, flags, sharing, permissions); }
    int __cdecl BridgeSOpenDispatch(const char* path, int flags, int sharing, int permissions, int* output, int)
    { return BridgeSOpenS(output, path, flags, sharing, permissions); }
    int __cdecl BridgeWCreat(const wchar_t* path, int permissions) { return OpenCrtFile(path, _O_CREAT | _O_TRUNC | _O_WRONLY, _SH_DENYNO, permissions); }
    int __cdecl BridgeCreat(const char* path, int permissions)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWCreat(wide.c_str(), permissions) : -1; }

    int __cdecl BridgeCloseDescriptor(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->Close(descriptor) : -1; }
    int __cdecl BridgeReadDescriptor(int descriptor, void* buffer, unsigned count) { auto files = CurrentCrtFiles(); return files ? files->Read(descriptor, buffer, count) : -1; }
    int __cdecl BridgeWriteDescriptor(int descriptor, const void* buffer, unsigned count) { auto files = CurrentCrtFiles(); return files ? files->Write(descriptor, buffer, count) : -1; }
    int __cdecl BridgeDupDescriptor(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->Duplicate(descriptor) : -1; }
    int __cdecl BridgeDup2Descriptor(int descriptor, int target)
    { auto files = CurrentCrtFiles(); if (target < 0) return Win32Bridge::Bridge::CrtDescriptorFailure(EBADF); return files ? files->Duplicate(descriptor, target) : -1; }
    intptr_t __cdecl BridgeGetOsfHandle(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->Handle(descriptor) : -1; }
    int __cdecl BridgeOpenOsfHandle(intptr_t handle, int flags) { auto files = CurrentCrtFiles(); return files ? files->Adopt(reinterpret_cast<HANDLE>(handle), flags) : -1; }
    int __cdecl BridgeIsatty(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->IsTerminal(descriptor) : 0; }
    __int64 __cdecl BridgeLSeek64(int descriptor, __int64 offset, int origin) { auto files = CurrentCrtFiles(); return files ? files->Seek(descriptor, offset, origin) : -1; }
    long __cdecl BridgeLSeek(int descriptor, long offset, int origin)
    {
        const auto position = BridgeLSeek64(descriptor, offset, origin);
        if (position > LONG_MAX) return Win32Bridge::Bridge::CrtDescriptorFailure(EOVERFLOW);
        return static_cast<long>(position);
    }
    __int64 __cdecl BridgeTell64(int descriptor) { return BridgeLSeek64(descriptor, 0, FILE_CURRENT); }
    long __cdecl BridgeTell(int descriptor) { return BridgeLSeek(descriptor, 0, FILE_CURRENT); }
    __int64 __cdecl BridgeFileLength64(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->Size(descriptor) : -1; }
    long __cdecl BridgeFileLength(int descriptor)
    { const auto size = BridgeFileLength64(descriptor); return size > LONG_MAX ? Win32Bridge::Bridge::CrtDescriptorFailure(EOVERFLOW) : static_cast<long>(size); }
    int __cdecl BridgeEof(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->Eof(descriptor) : -1; }
    int __cdecl BridgeCommit(int descriptor) { auto files = CurrentCrtFiles(); return files ? files->Commit(descriptor) : -1; }
    int __cdecl BridgeChSize(int descriptor, long size) { auto files = CurrentCrtFiles(); return files ? files->Resize(descriptor, size) : -1; }
    int __cdecl BridgeChSizeS(int descriptor, __int64 size) { auto files = CurrentCrtFiles(); return files && files->Resize(descriptor, size) == 0 ? 0 : errno; }
    int __cdecl BridgeSetMode(int descriptor, int mode) { auto files = CurrentCrtFiles(); return files ? files->SetMode(descriptor, mode) : -1; }

    Win32Bridge::Bridge::GuestStorageContext* CrtPathStorage()
    {
        auto storage = Win32Bridge::Bridge::CurrentGuestStorageContext();
        if (!storage) Win32Bridge::Bridge::CrtDescriptorFailure(EBADF);
        return storage;
    }
    int __cdecl BridgeWAccess(const wchar_t* path, int mode)
    {
        using namespace Win32Bridge::Bridge;
        if (!path || (mode != 0 && mode != 2 && mode != 4 && mode != 6)) return CrtDescriptorFailure(EINVAL);
        auto storage = CrtPathStorage(); if (!storage) return -1;
        DWORD error = ERROR_SUCCESS;
        const DWORD attributes = storage->GetGuestFileAttributes(path, &error);
        if (attributes == INVALID_FILE_ATTRIBUTES) return CrtIoFailure(error);
        if ((mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return CrtDescriptorFailure(EACCES);
        return 0;
    }
    int __cdecl BridgeAccess(const char* path, int mode)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWAccess(wide.c_str(), mode) : -1; }
    int __cdecl BridgeWAccessS(const wchar_t* path, int mode) { return BridgeWAccess(path, mode) == 0 ? 0 : errno; }
    int __cdecl BridgeAccessS(const char* path, int mode) { return BridgeAccess(path, mode) == 0 ? 0 : errno; }
    int __cdecl BridgeWChmod(const wchar_t* path, int mode)
    {
        using namespace Win32Bridge::Bridge;
        if (!path || (mode & ~0600)) return CrtDescriptorFailure(EINVAL);
        auto storage = CrtPathStorage(); if (!storage) return -1;
        DWORD error = ERROR_SUCCESS; DWORD attributes = storage->GetGuestFileAttributes(path, &error);
        if (attributes == INVALID_FILE_ATTRIBUTES) return CrtIoFailure(error);
        attributes = mode & 0200 ? attributes & ~FILE_ATTRIBUTE_READONLY : attributes | FILE_ATTRIBUTE_READONLY;
        return storage->SetGuestFileAttributes(path, attributes, &error) ? 0 : CrtIoFailure(error);
    }
    int __cdecl BridgeChmod(const char* path, int mode)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWChmod(wide.c_str(), mode) : -1; }
    int __cdecl BridgeWUnlink(const wchar_t* path)
    {
        auto storage = CrtPathStorage(); if (!storage) return -1;
        if (!path) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS;
        return storage->DeleteGuestFile(path, &error) ? 0 : Win32Bridge::Bridge::CrtIoFailure(error);
    }
    int __cdecl BridgeUnlink(const char* path)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWUnlink(wide.c_str()) : -1; }
    int __cdecl BridgeWRename(const wchar_t* from, const wchar_t* to)
    {
        auto storage = CrtPathStorage(); if (!storage) return -1;
        if (!from || !to) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS;
        return storage->MoveGuestPath(from, to, 0, &error) ? 0 : Win32Bridge::Bridge::CrtIoFailure(error);
    }
    int __cdecl BridgeRename(const char* from, const char* to)
    { std::wstring wideFrom, wideTo; return CrtFileWidePath(from, &wideFrom) && CrtFileWidePath(to, &wideTo) ? BridgeWRename(wideFrom.c_str(), wideTo.c_str()) : -1; }
    int __cdecl BridgeWMkdir(const wchar_t* path)
    {
        auto storage = CrtPathStorage(); if (!storage) return -1;
        if (!path) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS;
        return storage->CreateDirectory(path, &error) ? 0 : Win32Bridge::Bridge::CrtIoFailure(error);
    }
    int __cdecl BridgeMkdir(const char* path)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWMkdir(wide.c_str()) : -1; }
    int __cdecl BridgeWRmdir(const wchar_t* path)
    {
        auto storage = CrtPathStorage(); if (!storage) return -1;
        if (!path) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS;
        return storage->RemoveGuestDirectory(path, &error) ? 0 : Win32Bridge::Bridge::CrtIoFailure(error);
    }
    int __cdecl BridgeRmdir(const char* path)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWRmdir(wide.c_str()) : -1; }
    int __cdecl BridgeWChdir(const wchar_t* path)
    {
        auto storage = CrtPathStorage(); if (!storage) return -1;
        if (!path) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS;
        return storage->SetCurrentDirectory(path, &error) ? 0 : Win32Bridge::Bridge::CrtIoFailure(error);
    }
    int __cdecl BridgeChdir(const char* path)
    { std::wstring wide; return CrtFileWidePath(path, &wide) ? BridgeWChdir(wide.c_str()) : -1; }
    template<class Character> Character* CopyCrtWorkingDirectory(const std::basic_string<Character>& path, Character* buffer, int count)
    {
        using namespace Win32Bridge::Bridge;
        if (count < 0 || (buffer && !count)) { CrtDescriptorFailure(EINVAL); return nullptr; }
        const size_t required = path.size() + 1;
        if (count && static_cast<size_t>(count) < required) { CrtDescriptorFailure(ERANGE); return nullptr; }
        if (!buffer)
        {
            const size_t capacity = count ? static_cast<size_t>(count) : required;
            if (capacity > (std::numeric_limits<size_t>::max)() / sizeof(Character)) { CrtDescriptorFailure(ENOMEM); return nullptr; }
            buffer = static_cast<Character*>(std::malloc(capacity * sizeof(Character)));
            if (!buffer) { CrtDescriptorFailure(ENOMEM); return nullptr; }
        }
        memcpy(buffer, path.c_str(), required * sizeof(Character)); return buffer;
    }
    wchar_t* __cdecl BridgeWGetCwd(wchar_t* buffer, int count)
    {
        auto storage = CrtPathStorage(); if (!storage) return nullptr;
        try { return CopyCrtWorkingDirectory(storage->CurrentDirectory(), buffer, count); }
        catch (const std::bad_alloc&) { Win32Bridge::Bridge::CrtDescriptorFailure(ENOMEM); return nullptr; }
    }
    char* __cdecl BridgeGetCwd(char* buffer, int count)
    {
        auto storage = CrtPathStorage(); if (!storage) return nullptr;
        try { return CopyCrtWorkingDirectory(Win32Bridge::Bridge::CrtNarrowString(storage->CurrentDirectory()), buffer, count); }
        catch (const std::bad_alloc&) { Win32Bridge::Bridge::CrtDescriptorFailure(ENOMEM); return nullptr; }
    }
    int __cdecl BridgeUmask(int mask)
    {
        auto state = CurrentCrt(); if (!state) return Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL);
        return static_cast<int>(::InterlockedExchange(reinterpret_cast<volatile LONG*>(&state->fileCreationMask), mask & 0600));
    }
    int __cdecl BridgeUmaskS(int mask, int* previous)
    {
        if (!previous || (mask & ~0600)) { Win32Bridge::Bridge::CrtDescriptorFailure(EINVAL); return EINVAL; }
        const int result = BridgeUmask(mask);
        if (result < 0) return errno;
        *previous = result; return 0;
    }
    int __cdecl BridgeCrtPipe(int* descriptors, unsigned capacity, int mode)
    {
        using namespace Win32Bridge::Bridge;
        if (!descriptors || (mode & ~(_O_TEXT | _O_BINARY | _O_NOINHERIT)) ||
            ((mode & _O_TEXT) && (mode & _O_BINARY))) return CrtDescriptorFailure(EINVAL);
        auto files = CurrentCrtFiles(); if (!files) return -1;
        descriptors[0] = descriptors[1] = -1;
        if (!(mode & (_O_TEXT | _O_BINARY)))
        {
            int defaultMode = _O_TEXT;
            if (BridgeGetFileMode(&defaultMode)) return -1;
            mode |= defaultMode & (_O_TEXT | _O_BINARY);
        }
        SECURITY_ATTRIBUTES security{}; security.nLength = sizeof(security);
        security.bInheritHandle = (mode & _O_NOINHERIT) == 0;
        HANDLE reader = nullptr, writer = nullptr;
        if (!BridgeCreatePipe(&reader, &writer, &security, capacity)) return CrtIoFailure(BridgeGetLastError());
        const int readFd = files->Adopt(reader, _O_RDONLY | mode, true);
        if (readFd < 0) { BridgeCloseHandle(writer); return -1; }
        const int writeFd = files->Adopt(writer, _O_WRONLY | mode, true);
        if (writeFd < 0) { files->Close(readFd); return -1; }
        descriptors[0] = readFd; descriptors[1] = writeFd; return 0;
    }
    Win32Bridge::Bridge::ImportResolution ResolveCrtFileImport(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
        if (symbol.importedByOrdinal || (!IsMsvcrt(symbol.library) && _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll"))) return result;
#define CRT_FILE(exportName, function) if (symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&function); else
        CRT_FILE("_pipe", BridgeCrtPipe)
        CRT_FILE("_umask", BridgeUmask)
        CRT_FILE("_umask_s", BridgeUmaskS)
        CRT_FILE("_access", BridgeAccess)
        CRT_FILE("_waccess", BridgeWAccess)
        CRT_FILE("_access_s", BridgeAccessS)
        CRT_FILE("_waccess_s", BridgeWAccessS)
        CRT_FILE("_chmod", BridgeChmod)
        CRT_FILE("_wchmod", BridgeWChmod)
        CRT_FILE("_unlink", BridgeUnlink)
        CRT_FILE("_wunlink", BridgeWUnlink)
        CRT_FILE("remove", BridgeUnlink)
        CRT_FILE("_wremove", BridgeWUnlink)
        CRT_FILE("rename", BridgeRename)
        CRT_FILE("_wrename", BridgeWRename)
        CRT_FILE("_mkdir", BridgeMkdir)
        CRT_FILE("_wmkdir", BridgeWMkdir)
        CRT_FILE("_rmdir", BridgeRmdir)
        CRT_FILE("_wrmdir", BridgeWRmdir)
        CRT_FILE("_chdir", BridgeChdir)
        CRT_FILE("_wchdir", BridgeWChdir)
        CRT_FILE("_getcwd", BridgeGetCwd)
        CRT_FILE("_wgetcwd", BridgeWGetCwd)
        CRT_FILE("_open", BridgeOpen)
        CRT_FILE("_wopen", BridgeWOpen)
        CRT_FILE("_sopen", BridgeSOpen)
        CRT_FILE("_wsopen", BridgeWSOpen)
        CRT_FILE("_sopen_s", BridgeSOpenS)
        CRT_FILE("_wsopen_s", BridgeWSOpenS)
        CRT_FILE("_sopen_dispatch", BridgeSOpenDispatch)
        CRT_FILE("_wsopen_dispatch", BridgeWSOpenDispatch)
        CRT_FILE("_creat", BridgeCreat)
        CRT_FILE("_wcreat", BridgeWCreat)
        CRT_FILE("_close", BridgeCloseDescriptor)
        CRT_FILE("_read", BridgeReadDescriptor)
        CRT_FILE("_write", BridgeWriteDescriptor)
        CRT_FILE("_dup", BridgeDupDescriptor)
        CRT_FILE("_dup2", BridgeDup2Descriptor)
        CRT_FILE("_get_osfhandle", BridgeGetOsfHandle)
        CRT_FILE("_open_osfhandle", BridgeOpenOsfHandle)
        CRT_FILE("_isatty", BridgeIsatty)
        CRT_FILE("_lseek", BridgeLSeek)
        CRT_FILE("_lseeki64", BridgeLSeek64)
        CRT_FILE("_tell", BridgeTell)
        CRT_FILE("_telli64", BridgeTell64)
        CRT_FILE("_filelength", BridgeFileLength)
        CRT_FILE("_filelengthi64", BridgeFileLength64)
        CRT_FILE("_eof", BridgeEof)
        CRT_FILE("_commit", BridgeCommit)
        CRT_FILE("_chsize", BridgeChSize)
        CRT_FILE("_chsize_s", BridgeChSizeS)
        CRT_FILE("_setmode", BridgeSetMode)
        { }
#undef CRT_FILE
        if (result.targetAddress)
        {
            result.disposition = ImportDisposition::NeedsBridge;
            result.note = L"CRT file descriptors backed by guest storage.";
        }
        return result;
    }
}