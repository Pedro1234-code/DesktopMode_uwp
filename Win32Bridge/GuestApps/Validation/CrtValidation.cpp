#include "Panel.h"
#include "Scratch.h"
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <malloc.h>
#include <new>
#include <stdexcept>
#include <typeinfo>
#include <process.h>

namespace
{
    int Heap(HWND)
    {
        auto block = static_cast<unsigned char*>(std::calloc(64, 1));
        if (!block) return __LINE__;
        const bool zero = block[17] == 0;
        block[17] = 73;
        auto resized = static_cast<unsigned char*>(std::realloc(block, 192));
        const bool preserved = resized && resized[17] == 73;
        std::free(resized ? resized : block);
        void* aligned = _aligned_malloc(113, 64);
        const bool address = aligned && (reinterpret_cast<UINT_PTR>(aligned) & 63) == 0;
        _aligned_free(aligned);
        return zero && preserved && address ? 0 : __LINE__;
    }

    int Formatting(HWND)
    {
        char bytes[48]{}, word[32]{};
        int number = 0;
        double amount = 0;
        const int written = std::snprintf(bytes, sizeof(bytes), "%s %04d %.2f", "guest", 7, 2.5);
        const int read = sscanf_s(bytes, "%31s %d %lf", word, static_cast<unsigned>(sizeof(word)), &number, &amount);
        return written == 15 && read == 3 && std::strcmp(word, "guest") == 0 && number == 7 && amount == 2.5
            ? 0 : __LINE__;
    }

    int Stdio(HWND)
    {
        wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, dir) || !Validation::CreateScratchFile(dir, L"crt", 0, path)) return __LINE__;
        FILE* stream = nullptr;
        const char payload[] = "CRT\x00round trip";
        bool correct = false;
        if (_wfopen_s(&stream, path, L"w+b") == 0 && stream)
        {
            char bytes[sizeof(payload)]{};
            correct = std::fwrite(payload, 1, sizeof(payload), stream) == sizeof(payload) &&
                std::fflush(stream) == 0 && _fseeki64(stream, 0, SEEK_SET) == 0 &&
                std::fread(bytes, 1, sizeof(bytes), stream) == sizeof(bytes) &&
                std::memcmp(bytes, payload, sizeof(bytes)) == 0 && _ftelli64(stream) == sizeof(bytes);
            std::fclose(stream);
        }
        DeleteFileW(path);
        return correct ? 0 : __LINE__;
    }

    struct Probe { virtual ~Probe() = default; };
    struct Derived : Probe { int data = 42; };
    struct Unwind { int& calls; ~Unwind() { ++calls; } };
    __declspec(noinline) void Throw(int& calls)
    {
        Unwind guard{calls};
        throw std::runtime_error("guest exception");
    }

    int Exceptions(HWND)
    {
        int calls = 0;
        std::exception_ptr saved;
        try { Throw(calls); } catch (...) { saved = std::current_exception(); }
        if (calls != 1 || !saved) return __LINE__;
        try { std::rethrow_exception(saved); }
        catch (const std::exception& e) { return std::strcmp(e.what(), "guest exception") == 0 ? 0 : __LINE__; }
        catch (...) { return __LINE__; }
        return __LINE__;
    }

    __declspec(noinline) Derived* Cast(Probe* value) { return dynamic_cast<Derived*>(value); }
    int Rtti(HWND)
    {
        Derived value;
        Probe other;
        return Cast(&value) == &value && Cast(&other) == nullptr && typeid(*static_cast<Probe*>(&value)) == typeid(Derived)
            ? 0 : __LINE__;
    }

    int Locale(HWND)
    {
        _locale_t locale = _create_locale(LC_ALL, "C");
        if (!locale) return __LINE__;
        char* tail = nullptr;
        const double number = _strtod_l("3.125!", &tail, locale);
        _free_locale(locale);
        wchar_t text[8]{};
        const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, "a\xc3\xa7", -1, text, 8);
        return number == 3.125 && *tail == '!' && units == 3 && text[1] == 0x00e7 ? 0 : __LINE__;
    }

    unsigned __stdcall Worker(void*)
    {
        errno = ERANGE;
        return errno == ERANGE ? 17u : 0u;
    }

    int ThreadState(HWND)
    {
        errno = EINVAL;
        HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Worker, nullptr, 0, nullptr));
        if (!thread) return __LINE__;
        DWORD code = 0;
        const bool correct = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 &&
            GetExitCodeThread(thread, &code) && code == 17 && errno == EINVAL;
        CloseHandle(thread);
        return correct ? 0 : __LINE__;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    Validation::Log log(L"CrtValidation");
    const Validation::Button choices[] = {{L"Heap and alignment", Heap}, {L"Formatting and scanning", Formatting},
        {L"Binary stdio", Stdio}, {L"C++ exceptions and unwind", Exceptions}, {L"RTTI and dynamic_cast", Rtti},
        {L"Locale and Unicode", Locale}, {L"CRT thread-local state", ThreadState}};
    Validation::Panel(instance, log, choices, ARRAYSIZE(choices)).Show(L"CRT validation");
    return 0;
}
