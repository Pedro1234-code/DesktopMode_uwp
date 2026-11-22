namespace
{
    using namespace Win32Bridge::Bridge;

    constexpr size_t CrtSpawnCommandLimit = 32767; // Includes the final null.

    bool ConvertCrtSpawnText(std::wstring* text, const wchar_t* source)
    {
        text->assign(source);
        return true;
    }

    bool ConvertCrtSpawnText(std::wstring* text, const char* source)
    {
        return CrtFileWidePath(source, text);
    }

    int CrtSpawnError(DWORD error)
    {
        if (error == ERROR_BAD_EXE_FORMAT || error == ERROR_BAD_FORMAT || error == ERROR_EXE_MACHINE_TYPE_MISMATCH)
        {
            errno = ENOEXEC;
            _set_doserrno(error);
            return -1;
        }
        return CrtIoFailure(error);
    }

    DWORD WaitForCrtChild(HANDLE process, DWORD* exitCode)
    {
        if (!BridgeGetExitCodeProcess(process, exitCode)) return BridgeGetLastError();
        if (BridgeWaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
            return BridgeGetLastError();
        return BridgeGetExitCodeProcess(process, exitCode) ? ERROR_SUCCESS : BridgeGetLastError();
    }

    intptr_t __cdecl CrtCWait(int* status, intptr_t child, int)
    {
        if (status) *status = -1;
        if (child == -1 || child == -2) return CrtDescriptorFailure(ECHILD);
        const HANDLE process = reinterpret_cast<HANDLE>(child);
        DWORD exitCode = 0;
        const DWORD error = WaitForCrtChild(process, &exitCode);
        BridgeCloseHandle(process);
        if (error == ERROR_INVALID_HANDLE)
        {
            errno = ECHILD;
            _set_doserrno(error);
            return -1;
        }
        if (error != ERROR_SUCCESS) return CrtSpawnError(error);
        if (status) *status = static_cast<int>(exitCode);
        return child;
    }

    bool MissingCrtExecutable(DWORD error)
    {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_MOD_NOT_FOUND;
    }

    bool LaunchCrtExecutable(GuestRuntime& runtime, const std::wstring& path, const std::wstring& command,
        const std::vector<std::wstring>* environment, DWORD flags, const std::vector<BYTE>& descriptors,
        PROCESS_INFORMATION* process, DWORD* error)
    {
        const size_t separator = path.find_last_of(L"\\/:");
        const size_t dot = path.find_last_of(L'.');
        const bool hasExtension = dot != std::wstring::npos && (separator == std::wstring::npos || dot > separator);
        if (hasExtension)
        {
            const auto executable = dot + 1 == path.size() ? path.substr(0, dot) : path;
            return runtime.LaunchChildProcess(executable.c_str(), command.c_str(), nullptr,
                process, error, environment, flags, true, nullptr, descriptors);
        }
        for (const auto extension : { L".com", L".exe", L".bat", L".cmd" })
        {
            const auto executable = path + extension;
            if (runtime.LaunchChildProcess(executable.c_str(), command.c_str(), nullptr,
                    process, error, environment, flags, true, nullptr, descriptors)) return true;
            if (!MissingCrtExecutable(*error)) break;
        }
        return false;
    }

    template <typename Char>
    intptr_t SpawnCrtProcess(int mode, const Char* path, const Char* const* argv,
        const Char* const* envp, bool searchPath)
    {
        if (!path || !*path || !argv || !argv[0] || !*argv[0] || mode < _P_WAIT || mode > _P_DETACH)
            return CrtDescriptorFailure(EINVAL);
        if (mode == _P_OVERLAY) return CrtDescriptorFailure(ENOTSUP);

        auto runtime = CurrentGuestRuntime();
        auto loader = CurrentGuestModuleLoader();
        if (!runtime || !loader) return CrtDescriptorFailure(EINVAL);
        const int savedErrno = errno;
        try
        {
            std::wstring filename, command, entry;
            if (!ConvertCrtSpawnText(&filename, path)) return -1;
            for (auto arg = argv; *arg; ++arg)
            {
                if (!ConvertCrtSpawnText(&entry, *arg)) return -1;
                const size_t separator = arg != argv;
                if (entry.size() + separator >= CrtSpawnCommandLimit - command.size())
                    return CrtDescriptorFailure(E2BIG);
                // The CRT accepts fragments of a Win32 command line. The
                // caller supplies any quotes needed to preserve whitespace.
                if (separator) command.push_back(L' ');
                command += entry;
            }

            std::vector<std::wstring> environment;
            if (envp)
            {
                for (const auto& variable : loader->Environment().Snapshot())
                    if (variable.size() >= 4 && variable[0] == L'=' && variable[2] == L':' && variable[3] == L'=')
                        environment.push_back(variable);
                bool hasSystemRoot = false;
                for (auto value = envp; *value; ++value)
                {
                    if (!ConvertCrtSpawnText(&entry, *value)) return -1;
                    if (_wcsnicmp(entry.c_str(), L"SystemRoot=", 11) == 0) hasSystemRoot = true;
                    environment.push_back(entry);
                }
                if (!hasSystemRoot && loader->Environment().Get(L"SystemRoot", &entry))
                    environment.push_back(L"SystemRoot=" + entry);
            }

            std::vector<BYTE> descriptors;
            const bool detached = mode == _P_DETACH;
            if (!loader->CrtFiles().BuildSpawnData(!detached, &descriptors)) return -1;
            constexpr DWORD DetachedProcess = 0x00000008u;
            const DWORD flags = detached ? DetachedProcess : 0;
            const auto explicitEnvironment = envp ? &environment : nullptr;
            const bool baseName = filename.find_first_of(L"\\/:") == std::wstring::npos;
            const auto local = baseName ? L".\\" + filename : filename;
            PROCESS_INFORMATION child{};
            DWORD error = ERROR_SUCCESS;
            bool launched = LaunchCrtExecutable(*runtime, local, command, explicitEnvironment, flags, descriptors, &child, &error);
            if (!launched && searchPath && baseName && MissingCrtExecutable(error))
            {
                for (auto directory : loader->Environment().SearchDirectories())
                {
                    if (directory.back() != L'\\' && directory.back() != L'/') directory.push_back(L'\\');
                    launched = LaunchCrtExecutable(*runtime, directory + filename, command,
                        explicitEnvironment, flags, descriptors, &child, &error);
                    if (launched || (!MissingCrtExecutable(error) && error != ERROR_NOT_READY)) break;
                }
            }
            if (!launched) return CrtSpawnError(error);

            BridgeCloseHandle(child.hThread);
            intptr_t result = reinterpret_cast<intptr_t>(child.hProcess);
            if (detached || mode == _P_WAIT)
            {
                DWORD exitCode = 0;
                error = detached ? ERROR_SUCCESS : WaitForCrtChild(child.hProcess, &exitCode);
                BridgeCloseHandle(child.hProcess);
                if (error != ERROR_SUCCESS) return CrtSpawnError(error);
                result = static_cast<int>(exitCode);
            }
            errno = savedErrno;
            _set_doserrno(0);
            return result;
        }
        catch (const std::bad_alloc&) { return CrtDescriptorFailure(ENOMEM); }
    }

    template <typename Char, bool SearchPath>
    intptr_t __cdecl CrtSpawnV(int mode, const Char* path, const Char* const* argv)
    {
        return SpawnCrtProcess<Char>(mode, path, argv, nullptr, SearchPath);
    }

    template <typename Char, bool SearchPath>
    intptr_t __cdecl CrtSpawnVE(int mode, const Char* path, const Char* const* argv, const Char* const* envp)
    {
        return SpawnCrtProcess(mode, path, argv, envp, SearchPath);
    }

    template <typename Char, bool SearchPath, bool HasEnvironment>
    intptr_t __cdecl CrtSpawnL(int mode, const Char* path, const Char* first, ...)
    {
        if (!first) return CrtDescriptorFailure(EINVAL);
        std::vector<const Char*> argv;
        const Char* const* envp = nullptr;
        va_list args;
        va_start(args, first);
        try
        {
            argv.push_back(first);
            while (const auto arg = va_arg(args, const Char*))
            {
                if (argv.size() >= CrtSpawnCommandLimit)
                { va_end(args); return CrtDescriptorFailure(E2BIG); }
                argv.push_back(arg);
            }
            if (HasEnvironment) envp = va_arg(args, const Char* const*);
            argv.push_back(nullptr);
        }
        catch (const std::bad_alloc&)
        { va_end(args); return CrtDescriptorFailure(ENOMEM); }
        va_end(args);
        return SpawnCrtProcess(mode, path, argv.data(), envp, SearchPath);
    }

    ULONGLONG ResolveCrtSpawnAddress(const std::wstring& name)
    {
#define CRT_SPAWN_EXPORT(exportName, ...) if (name == L##exportName) return reinterpret_cast<ULONGLONG>(&__VA_ARGS__);
        CRT_SPAWN_EXPORT("_spawnv", CrtSpawnV<char, false>)
        CRT_SPAWN_EXPORT("_spawnve", CrtSpawnVE<char, false>)
        CRT_SPAWN_EXPORT("_spawnvp", CrtSpawnV<char, true>)
        CRT_SPAWN_EXPORT("_spawnvpe", CrtSpawnVE<char, true>)
        CRT_SPAWN_EXPORT("_spawnl", CrtSpawnL<char, false, false>)
        CRT_SPAWN_EXPORT("_spawnle", CrtSpawnL<char, false, true>)
        CRT_SPAWN_EXPORT("_spawnlp", CrtSpawnL<char, true, false>)
        CRT_SPAWN_EXPORT("_spawnlpe", CrtSpawnL<char, true, true>)
        CRT_SPAWN_EXPORT("_wspawnv", CrtSpawnV<wchar_t, false>)
        CRT_SPAWN_EXPORT("_wspawnve", CrtSpawnVE<wchar_t, false>)
        CRT_SPAWN_EXPORT("_wspawnvp", CrtSpawnV<wchar_t, true>)
        CRT_SPAWN_EXPORT("_wspawnvpe", CrtSpawnVE<wchar_t, true>)
        CRT_SPAWN_EXPORT("_wspawnl", CrtSpawnL<wchar_t, false, false>)
        CRT_SPAWN_EXPORT("_wspawnle", CrtSpawnL<wchar_t, false, true>)
        CRT_SPAWN_EXPORT("_wspawnlp", CrtSpawnL<wchar_t, true, false>)
        CRT_SPAWN_EXPORT("_wspawnlpe", CrtSpawnL<wchar_t, true, true>)
        CRT_SPAWN_EXPORT("_cwait", CrtCWait)
#undef CRT_SPAWN_EXPORT
        return 0;
    }
}
