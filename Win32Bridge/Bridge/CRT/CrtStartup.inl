// Included by MsvcrtShims.cpp after the core adapters.
namespace
{
    void __cdecl BridgeRegisterExeTlsExit(void(__stdcall* callback)(void*, DWORD, void*))
    {
        auto state = CurrentCrt();
        if (!state) { Win32Bridge::Bridge::TerminateCrtExecution(); }
        bool duplicate = false;
        { std::lock_guard<std::recursive_mutex> guard(state->ExitMutex()); duplicate = state->executableTlsExitRegistered; if (!duplicate) { state->executableTlsExit = callback; state->executableTlsExitRegistered = callback != nullptr; } }
        if (duplicate) Win32Bridge::Bridge::TerminateCrtExecution();
    }
    int ConfigureCrtArguments(bool wide, int mode)
    {
        if (mode == 0) return 0; // _crt_argv_no_arguments leaves existing state alone.
        if (mode != 1 && mode != 2) return EINVAL;
        auto state = CurrentCrt();
        if (!state) return EINVAL;
        std::lock_guard<std::recursive_mutex> guard(state->ExitMutex());
        if (state->ArgumentMode(wide) == mode) { state->argumentCount = state->ArgumentCount(wide); return 0; }
        try
        {
            auto arguments = Win32Bridge::Bridge::ParseCrtCommandLine(state->wideCommandLine);
            if (mode == 2)
            {
                auto storage = Win32Bridge::Bridge::CurrentGuestStorageContext();
                if (!storage) return EINVAL;
                std::vector<std::wstring> expanded;
                for (size_t index = 0; index < arguments.size(); ++index)
                {
                    const auto& argument = arguments[index];
                    const size_t wildcard = argument.find_first_of(L"*?");
                    if (!index || wildcard == std::wstring::npos) { expanded.push_back(argument); continue; }
                    const size_t separator = argument.find_last_of(L"\\/:", wildcard);
                    if (separator != std::wstring::npos && argument[separator] == L':' && separator != 1)
                    { expanded.push_back(argument); continue; }
                    const auto prefix = separator == std::wstring::npos ? std::wstring() : argument.substr(0, separator + 1);
                    WIN32_FIND_DATAW data{}; HANDLE handle = INVALID_HANDLE_VALUE; DWORD error = ERROR_SUCCESS;
                    if (!storage->FindFirstGuestFile(argument.c_str(), &data, &handle, &error))
                    { expanded.push_back(argument); continue; }
                    struct FindScope
                    {
                        Win32Bridge::Bridge::GuestStorageContext* storage; HANDLE handle;
                        ~FindScope() { DWORD ignored; storage->CloseFindHandle(handle, &ignored); }
                    } findScope{ storage, handle };
                    const size_t begin = expanded.size();
                    do
                    {
                        if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L".."))
                            expanded.push_back(prefix + data.cFileName);
                    } while (storage->FindNextGuestFile(handle, &data, &error));
                    std::sort(expanded.begin() + begin, expanded.end(), [](const std::wstring& a, const std::wstring& b)
                    { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
                }
                arguments = std::move(expanded);
            }
            state->ConfigureArguments(wide, mode, std::move(arguments));
            return 0;
        }
        catch (const std::bad_alloc&) { return ENOMEM; }
    }
    int __cdecl BridgeConfigureNarrowArguments(int mode) { return ConfigureCrtArguments(false, mode); }
    int __cdecl BridgeConfigureWideArguments(int mode) { return ConfigureCrtArguments(true, mode); }
    using CrtOnExitFunction = int(__cdecl*)();
    using Win32Bridge::Bridge::CrtOnExitTable;
    using Win32Bridge::Bridge::CrtProcessState;
    static_assert(sizeof(CrtOnExitTable) == sizeof(_onexit_table_t), "CRT onexit table ABI mismatch");
    static_assert(offsetof(CrtOnExitTable, end) == offsetof(_onexit_table_t, _end), "CRT onexit table layout mismatch");

    CrtOnExitFunction __cdecl BridgeDllOnExit(CrtOnExitFunction callback, CrtOnExitFunction** first, CrtOnExitFunction** last)
    {
        auto state = CurrentCrt();
        if (!state || !callback || !first || !last || (!*first != !*last)) return nullptr;
        std::lock_guard<std::recursive_mutex> guard(state->ExitMutex());
        CrtOnExitTable table{ reinterpret_cast<CrtProcessState::ExitCallback*>(*first),
            reinterpret_cast<CrtProcessState::ExitCallback*>(*last), reinterpret_cast<CrtProcessState::ExitCallback*>(*last) };
        if (Win32Bridge::Bridge::RegisterCrtOnExitTable(&table, reinterpret_cast<CrtProcessState::ExitCallback>(callback))) return nullptr;
        *first = reinterpret_cast<CrtOnExitFunction*>(table.first);
        *last = reinterpret_cast<CrtOnExitFunction*>(table.last);
        return callback;
    }
    int __cdecl BridgeInitializeOnExitTable(CrtOnExitTable* table)
    {
        auto state = CurrentCrt(); if (!state) return -1;
        std::lock_guard<std::recursive_mutex> guard(state->ExitMutex());
        return Win32Bridge::Bridge::InitializeCrtOnExitTable(table);
    }
    int __cdecl BridgeRegisterOnExitFunction(CrtOnExitTable* table, CrtOnExitFunction callback)
    {
        auto state = CurrentCrt(); if (!state) return -1;
        std::lock_guard<std::recursive_mutex> guard(state->ExitMutex());
        return Win32Bridge::Bridge::RegisterCrtOnExitTable(table, reinterpret_cast<CrtProcessState::ExitCallback>(callback));
    }
    int __cdecl BridgeExecuteOnExitTable(CrtOnExitTable* table)
    {
        auto state = CurrentCrt(); if (!state) return -1;
        return Win32Bridge::Bridge::ExecuteCrtOnExitTable(table, &state->ExitMutex());
    }
    int __cdecl BridgeAtQuickExit(CrtProcessState::ExitCallback callback)
    {
        auto state = CurrentCrt();
        return state && state->Register(callback, true) ? 0 : -1;
    }
    void __cdecl BridgeQuickExit(int code)
    {
        if (auto state = CurrentCrt()) state->Execute(true);
        BridgeImmediateExit(code);
    }
    int __cdecl BridgeQueryAppType() { auto state = CurrentCrt(); return state ? state->appType : 0; }
    int __cdecl BridgeGetFileMode(int* mode)
    {
        auto state = CurrentCrt();
        if (!state || !mode) { errno = EINVAL; return EINVAL; }
        *mode = static_cast<int>(::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&state->fileMode), 0, 0));
        return 0;
    }
    int __cdecl BridgeSetFileMode(int mode)
    {
        auto state = CurrentCrt();
        if (!state || (mode != 0x4000 && mode != 0x8000 && mode != 0x10000)) { errno = EINVAL; return EINVAL; }
        ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&state->fileMode), mode);
        return 0;
    }
    int __cdecl BridgeGetProgramName(char** name)
    {
        auto state = CurrentCrt();
        if (!state || !name) { errno = EINVAL; return EINVAL; }
        *name = state->narrowProgramName; return 0;
    }
    int __cdecl BridgeGetWideProgramName(wchar_t** name)
    {
        auto state = CurrentCrt();
        if (!state || !name) { errno = EINVAL; return EINVAL; }
        *name = state->wideProgramName; return 0;
    }
    int __cdecl BridgeInitializeEnvironment() { return CurrentCrt() ? 0 : -1; }
    char** __cdecl BridgeInitialNarrowEnvironment() { auto state = CurrentCrt(); return state ? state->narrowEnvironment : nullptr; }
    wchar_t** __cdecl BridgeInitialWideEnvironment() { auto state = CurrentCrt(); return state ? state->wideEnvironment : nullptr; }

#define CRT_POINTER_ACCESSOR(function, type, member) \
    type __cdecl function() { auto state = CurrentCrt(); return state ? &state->member : nullptr; }
    CRT_POINTER_ACCESSOR(BridgeArgcPointer, int*, argumentCount)
    CRT_POINTER_ACCESSOR(BridgeArgvPointer, char***, narrowArguments)
    CRT_POINTER_ACCESSOR(BridgeWArgvPointer, wchar_t***, wideArguments)
    CRT_POINTER_ACCESSOR(BridgeEnvironPointer, char***, narrowEnvironment)
    CRT_POINTER_ACCESSOR(BridgeWEnvironPointer, wchar_t***, wideEnvironment)
    CRT_POINTER_ACCESSOR(BridgeCommandPointer, char**, narrowCommandLine)
    CRT_POINTER_ACCESSOR(BridgeWCommandPointer, wchar_t**, wideCommandLine)
    CRT_POINTER_ACCESSOR(BridgeProgramPointer, char**, narrowProgramName)
    CRT_POINTER_ACCESSOR(BridgeWProgramPointer, wchar_t**, wideProgramName)
    CRT_POINTER_ACCESSOR(BridgeFileModePointer, int*, fileMode)
    CRT_POINTER_ACCESSOR(BridgeCommitModePointer, int*, commitMode)
#undef CRT_POINTER_ACCESSOR

    template<class Character> Character* CrtWinMainTail(Character* command)
    {
        if (!command) return nullptr;
        bool quoted = false;
        while (*command)
        {
            if (*command == '"') quoted = !quoted;
            if (!quoted && (*command == ' ' || *command == '\t')) break;
            ++command;
        }
        while (*command == ' ' || *command == '\t') ++command;
        return command;
    }
    char* __cdecl BridgeNarrowWinMainCommand() { auto state = CurrentCrt(); return state ? CrtWinMainTail(state->narrowCommandLine) : nullptr; }
    wchar_t* __cdecl BridgeWideWinMainCommand() { auto state = CurrentCrt(); return state ? CrtWinMainTail(state->wideCommandLine) : nullptr; }

    Win32Bridge::Bridge::ImportResolution ResolveCrtProcessImport(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
        if (symbol.importedByOrdinal || (!IsMsvcrt(symbol.library) && _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll"))) return result;
        auto state = CurrentCrt();
#define CRT_FUNCTION(exportName, function) if (symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&function); else
        CRT_FUNCTION("_register_thread_local_exe_atexit_callback", BridgeRegisterExeTlsExit)
        CRT_FUNCTION("_set_app_type", BridgeSetAppType)
        CRT_FUNCTION("__set_app_type", BridgeSetAppType)
        CRT_FUNCTION("_query_app_type", BridgeQueryAppType)
        CRT_FUNCTION("_get_fmode", BridgeGetFileMode)
        CRT_FUNCTION("_set_fmode", BridgeSetFileMode)
        CRT_FUNCTION("_get_pgmptr", BridgeGetProgramName)
        CRT_FUNCTION("_get_wpgmptr", BridgeGetWideProgramName)
        CRT_FUNCTION("_configure_narrow_argv", BridgeConfigureNarrowArguments)
        CRT_FUNCTION("_configure_wide_argv", BridgeConfigureWideArguments)
        CRT_FUNCTION("__getmainargs", BridgeGetMainArgs)
        CRT_FUNCTION("__wgetmainargs", BridgeWGetMainArgs)
        CRT_FUNCTION("exit", BridgeExit)
        CRT_FUNCTION("_exit", BridgeImmediateExit)
        CRT_FUNCTION("_Exit", BridgeImmediateExit)
        CRT_FUNCTION("quick_exit", BridgeQuickExit)
        CRT_FUNCTION("_cexit", BridgeCExit)
        CRT_FUNCTION("_c_exit", BridgeQuickCExit)
        CRT_FUNCTION("atexit", BridgeAtExit)
        CRT_FUNCTION("_crt_atexit", BridgeAtExit)
        CRT_FUNCTION("_onexit", BridgeOnExit)
        CRT_FUNCTION("_crt_at_quick_exit", BridgeAtQuickExit)
        CRT_FUNCTION("at_quick_exit", BridgeAtQuickExit)
        CRT_FUNCTION("__dllonexit", BridgeDllOnExit)
        CRT_FUNCTION("_initialize_onexit_table", BridgeInitializeOnExitTable)
        CRT_FUNCTION("_register_onexit_function", BridgeRegisterOnExitFunction)
        CRT_FUNCTION("_execute_onexit_table", BridgeExecuteOnExitTable)
        CRT_FUNCTION("_initialize_narrow_environment", BridgeInitializeEnvironment)
        CRT_FUNCTION("_initialize_wide_environment", BridgeInitializeEnvironment)
        CRT_FUNCTION("_get_initial_narrow_environment", BridgeInitialNarrowEnvironment)
        CRT_FUNCTION("_get_initial_wide_environment", BridgeInitialWideEnvironment)
        CRT_FUNCTION("_get_narrow_winmain_command_line", BridgeNarrowWinMainCommand)
        CRT_FUNCTION("_get_wide_winmain_command_line", BridgeWideWinMainCommand)
        CRT_FUNCTION("__p___argc", BridgeArgcPointer)
        CRT_FUNCTION("__p___argv", BridgeArgvPointer)
        CRT_FUNCTION("__p___wargv", BridgeWArgvPointer)
        CRT_FUNCTION("__p__environ", BridgeEnvironPointer)
        CRT_FUNCTION("__p__wenviron", BridgeWEnvironPointer)
        CRT_FUNCTION("__p__acmdln", BridgeCommandPointer)
        CRT_FUNCTION("__p__wcmdln", BridgeWCommandPointer)
        CRT_FUNCTION("__p__pgmptr", BridgeProgramPointer)
        CRT_FUNCTION("__p__wpgmptr", BridgeWProgramPointer)
        CRT_FUNCTION("__p__fmode", BridgeFileModePointer)
        CRT_FUNCTION("__p__commode", BridgeCommitModePointer)
        { }
#undef CRT_FUNCTION
#define CRT_DATA(exportName, member) if (state && symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&state->member);
        CRT_DATA("__argc", argumentCount)
        CRT_DATA("__argv", narrowArguments)
        CRT_DATA("__wargv", wideArguments)
        CRT_DATA("_environ", narrowEnvironment)
        CRT_DATA("_wenviron", wideEnvironment)
        CRT_DATA("_acmdln", narrowCommandLine)
        CRT_DATA("_wcmdln", wideCommandLine)
        CRT_DATA("_pgmptr", narrowProgramName)
        CRT_DATA("_wpgmptr", wideProgramName)
        CRT_DATA("_fmode", fileMode)
        CRT_DATA("_commode", commitMode)
#undef CRT_DATA
        if (result.targetAddress)
        {
            result.disposition = ImportDisposition::NeedsBridge;
            result.note = L"Process-local CRT startup and termination.";
        }
        return result;
    }
}