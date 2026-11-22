#include "CrtLocaleFixtures.h"
#include "CrtMultibyteFixtures.h"
#include "CrtThreadStateFixtures.h"
#include "CrtHeapAdapterFixtures.h"
#include "CrtRttiFixtures.h"
#include "X64UnwindFixtures.h"
#include "CrtExceptionPolicyFixtures.h"
#include "CrtExceptionPtrFixtures.h"
#include "CrtHeapFixtures.h"
#include "PipeFixtures.h"
#include "ConsoleFixtures.h"
#include "ConsoleBufferFixtures.h"
#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge/ExportAliases.h"
#include "Bridge/MemoryStreams.h"
#include "Bridge/ComIdentifiers.h"
#include "Bridge/AutomationData.h"
#include "Bridge/GDI/RasterGraphics.h"
#include "Bridge/CRT/CrtData.h"
#include "CrtFileFixtures.h"
#include "CrtStdioFixtures.h"
#include "CrtStreamFixtures.h"
#include "CrtFormattingFixtures.h"
#include "CrtScanningFixtures.h"
#include "CrtExceptionFixtures.h"
#include "Bridge/CRT/CrtProcessState.h"
#include "Bridge/ProcessEnvironment.h"
#include <memory>
#include "Bridge\\ImportBinder.h"
#include "Bridge\\PeImage.h"
#include "Bridge\\PeMapper.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <cstring>

namespace
{
    struct ResolverFixture
    {
        const wchar_t* description;
        const wchar_t* library;
        const wchar_t* name;
        Win32Bridge::Bridge::ImportDisposition expectedDisposition;
    };

    std::vector<int> g_crtExitOrder;
    Win32Bridge::Bridge::CrtProcessState* g_crtExitState = nullptr;
    Win32Bridge::Bridge::CrtOnExitTable* g_crtExitTable = nullptr;
    void __cdecl CrtExitFirst() { g_crtExitOrder.push_back(1); }
    void __cdecl CrtExitSecond() { g_crtExitOrder.push_back(2); }
    void __cdecl CrtExitRegistersAnother()
    {
        g_crtExitOrder.push_back(3);
        g_crtExitState->Register(&CrtExitSecond);
    }
    void __cdecl CrtTableRegistersAnother()
    {
        g_crtExitOrder.push_back(3);
        for (int index = 0; index < 8; ++index)
            Win32Bridge::Bridge::RegisterCrtOnExitTable(g_crtExitTable, &CrtExitSecond);
    }
    void __cdecl CrtTableReenters()
    {
        g_crtExitOrder.push_back(4);
        Win32Bridge::Bridge::ExecuteCrtOnExitTable(g_crtExitTable);
    }

    bool ValidateProcessEnvironmentFixtures()
    {
        using namespace Win32Bridge::Bridge;
        ProcessEnvironment parent, child, empty;
        parent.Initialize({ L"TEMP=C:\\Temp", L"MixedCase=value", L"EMPTY=", L"=C:=C:\\Work", L"invalid", L"mixedcase=duplicate" });
        std::wstring value;
        if (!parent.Get(L"mixedCASE", &value) || value != L"value" ||
            !parent.Get(L"empty", &value) || !value.empty() || parent.Get(L"missing", &value) ||
            !parent.Get(L"=c:", &value) || value != L"C:\\Work") return false;
        if (parent.Expand(L"%temp%\\%MixedCase%/%UNKNOWN%/%%/%") != L"C:\\Temp\\value/%UNKNOWN%/%%/%") return false;
        if (parent.Set(L"NESTED", L"%TEMP%") || parent.Expand(L"%NESTED%") != L"%TEMP%") return false;
        child.Initialize(parent.Snapshot());
        const auto savedBlock = child.Block();
        if (parent.Set(L"mixedcase", L"changed") || !child.Get(L"MIXEDCASE", &value) || value != L"value") return false;
        if (parent.Set(L"MixedCase", nullptr) || parent.Get(L"mixedcase", &value) ||
            parent.Set(L"does-not-exist", nullptr) || child.Block() != savedBlock) return false;
        if (parent.Set(L"EMPTY", L"") || !parent.Get(L"EMPTY", &value) || !value.empty()) return false;
        if (parent.Set(nullptr, L"x") != ERROR_INVALID_PARAMETER || parent.Set(L"", L"x") != ERROR_INVALID_PARAMETER ||
            parent.Set(L"bad=name", L"x") != ERROR_INVALID_PARAMETER) return false;
        const std::wstring excessive(32768, L'x');
        if (parent.Set(L"LENGTH", excessive.c_str()) != ERROR_INVALID_PARAMETER) return false;
        if (empty.Block() != std::vector<wchar_t>({ 0, 0 }) || empty.Expand(L"%UNKNOWN%") != L"%UNKNOWN%") return false;
        if (parent.Set(L"PATH", L";\"C:\\A;B\";;C:\\Other;")) return false;
        if (parent.SearchDirectories() != std::vector<std::wstring>({ L"C:\\A;B", L"C:\\Other" })) return false;
        const auto snapshot = child.Snapshot();
        std::vector<std::wstring> fromBlock;
        for (const wchar_t* entry = savedBlock.data(); *entry; entry += wcslen(entry) + 1) fromBlock.emplace_back(entry);
        return fromBlock == snapshot && savedBlock.size() > 1 && savedBlock[savedBlock.size() - 1] == 0 && savedBlock[savedBlock.size() - 2] == 0;
    }
    bool ValidateCrtProcessFixtures()
    {
        using namespace Win32Bridge::Bridge;
        struct Fixture { const wchar_t* command; std::vector<std::wstring> arguments; };
        const Fixture fixtures[] = {
            { L"", { L"" } },
            { L"app.exe", { L"app.exe" } },
            { L"\"C:\\Program Files\\app.exe\"  a\t b", { L"C:\\Program Files\\app.exe", L"a", L"b" } },
            { L"app \"\" \"a b\" c", { L"app", L"", L"a b", L"c" } },
            { LR"(app "ab"cd ef)", { L"app", L"abcd", L"ef" } },
            { LR"(app "a""b" "unterminated)", { L"app", L"a\"b", L"unterminated" } },
            { LR"(app a\\b d\"e "f\\")", { L"app", LR"(a\\b)", L"d\"e", L"f\\" } },
            { L"  argument", { L"", L"argument" } },
        };
        for (const auto& fixture : fixtures)
            if (ParseCrtCommandLine(fixture.command) != fixture.arguments) return false;

        CrtProcessState first, second;
        first.Initialize(L"app \"hello world\"", { L"TEMP=C:\\Temp", L"EMPTY=" }, L"C:\\Apps\\app.exe");
        second.Initialize(L"other.exe", {});
        if (first.argumentCount != 2 || second.argumentCount != 1 || first.narrowArguments[2] ||
            first.wideArguments[2] || first.narrowEnvironment[2] || first.wideEnvironment[2] ||
            second.narrowEnvironment[0] || second.wideEnvironment[0]) return false;
        if (std::strcmp(first.narrowArguments[1], "hello world") ||
            std::wcscmp(first.wideProgramName, L"C:\\Apps\\app.exe") ||
            std::strcmp(first.narrowEnvironment[1], "EMPTY=")) return false;
        first.narrowArguments[1][0] = 'H';
        if (std::strcmp(first.narrowArguments[1], "Hello world") || second.argumentCount != 1) return false;

        first.ConfigureArguments(true, 2, { L"app", L"a.txt", L"b.txt" });
        if (first.argumentCount != 3 || first.ArgumentMode(true) != 2 ||
            first.wideArguments[3] || std::wcscmp(first.wideArguments[2], L"b.txt") ||
            std::strcmp(first.narrowArguments[1], "Hello world")) return false;
        first.ConfigureArguments(false, 2, { L"app", L"a.txt", L"b.txt" });
        if (first.narrowArguments[3] || std::strcmp(first.narrowArguments[2], "b.txt")) return false;
        g_crtExitOrder.clear(); g_crtExitState = &first;
        if (!first.Register(&CrtExitFirst) || !first.Register(&CrtExitRegistersAnother) ||
            !second.Register(&CrtExitSecond) || !first.Register(&CrtExitSecond, true)) return false;
        first.Execute(); first.Execute();
        if (g_crtExitOrder != std::vector<int>({ 3, 2, 1 })) return false;
        first.Execute(true); second.Execute();
        if (g_crtExitOrder != std::vector<int>({ 3, 2, 1, 2, 2 })) return false;
        g_crtExitState = nullptr;

        CrtOnExitTable table{}; g_crtExitTable = &table; g_crtExitOrder.clear();
        if (InitializeCrtOnExitTable(nullptr) != -1 || RegisterCrtOnExitTable(nullptr, &CrtExitFirst) != -1 ||
            ExecuteCrtOnExitTable(nullptr) != -1 || InitializeCrtOnExitTable(&table)) return false;
        if (RegisterCrtOnExitTable(&table, &CrtExitFirst) ||
            RegisterCrtOnExitTable(&table, &CrtTableRegistersAnother) ||
            RegisterCrtOnExitTable(&table, nullptr)) return false;
        if (InitializeCrtOnExitTable(&table) || ExecuteCrtOnExitTable(&table)) return false;
        if (g_crtExitOrder != std::vector<int>({ 3, 2, 2, 2, 2, 2, 2, 2, 2, 1 }) || table.first || table.last || table.end) return false;
        g_crtExitOrder.clear();
        if (RegisterCrtOnExitTable(&table, &CrtExitFirst) || RegisterCrtOnExitTable(&table, &CrtTableReenters) ||
            ExecuteCrtOnExitTable(&table) || ExecuteCrtOnExitTable(&table)) return false;
        g_crtExitTable = nullptr;
        return g_crtExitOrder == std::vector<int>({ 4, 1 });
    }
    bool ValidateCrtDataFixtures()
    {
        using namespace Win32Bridge::Bridge;
        const auto resolve = [](const wchar_t* library, const wchar_t* name)
        {
            ImportedSymbol symbol;
            symbol.library = library; symbol.name = name;
            return ResolveCrtDataImport(symbol).targetAddress;
        };
        const auto allocate = reinterpret_cast<void*(__cdecl*)(size_t, size_t)>(resolve(L"ucrtbase.dll", L"calloc"));
        const auto freeMemory = reinterpret_cast<void(__cdecl*)(void*)>(resolve(L"msvcrt.dll", L"free"));
        const auto duplicate = reinterpret_cast<char*(__cdecl*)(const char*)>(resolve(L"ucrtbase.dll", L"_strdup"));
        const auto squareRoot = reinterpret_cast<double(__cdecl*)(double)>(resolve(L"UCRTBASE.DLL", L"sqrt"));
        if (!allocate || !freeMemory || !duplicate || !squareRoot) return false;
        auto bytes = static_cast<unsigned char*>(allocate(8, 1));
        if (!bytes) return false;
        bool zero = true;
        for (size_t index = 0; index < 8; ++index) zero = zero && bytes[index] == 0;
        freeMemory(bytes);
        if (!zero || squareRoot(4.0) != 2.0) return false;
        char* text = duplicate("guest runtime");
        if (!text) return false;
        const bool copied = std::strcmp(text, "guest runtime") == 0;
        freeMemory(text);
        if (!copied) return false;
        const auto aligned = reinterpret_cast<void*(__cdecl*)(size_t, size_t)>(resolve(L"ucrtbase.dll", L"_aligned_malloc"));
        const auto freeAligned = reinterpret_cast<void(__cdecl*)(void*)>(resolve(L"ucrtbase.dll", L"_aligned_free"));
        if (!aligned || !freeAligned) return false;
        void* memory = aligned(129, 64);
        if (!memory) return false;
        const bool alignment = reinterpret_cast<ULONG_PTR>(memory) % 64 == 0;
        freeAligned(memory);
        if (!alignment) return false;
        const auto integer = reinterpret_cast<long(__cdecl*)(const char*, char**, int)>(resolve(L"ucrtbase.dll", L"strtol"));
        if (!integer) return false;
        char* end = nullptr;
        if (integer("7f:end", &end, 16) != 127 || !end || std::strcmp(end, ":end") != 0) return false;
        const auto copy = reinterpret_cast<errno_t(__cdecl*)(char*, size_t, const char*)>(resolve(L"ucrtbase.dll", L"strcpy_s"));
        char output[16]{};
        if (!copy || copy(output, sizeof(output), "copied") != 0 || std::strcmp(output, "copied") != 0) return false;
        for (const auto name : {L"fopen", L"_open", L"exit", L"setlocale", L"_CxxThrowException", L"Malloc"})
            if (resolve(L"ucrtbase.dll", name)) return false;
        return !resolve(L"user32.dll", L"calloc");
    }
    bool ValidateDcStateFixtures()
    {
        using namespace Win32Bridge::Bridge::MiniGdi;
        GdiContext gdi;
        Surface surface(8, 8, OpaqueWhite);
        const auto dc = gdi.CreateDc(&surface);
        const auto red = MakeColor(255, 0, 0);
        const auto blue = MakeColor(0, 0, 255);
        const auto firstBrush = gdi.CreateSolidBrush(red);
        const auto secondBrush = gdi.CreateSolidBrush(blue);
        gdi.SelectBrush(dc, firstBrush);
        gdi.SetTextColor(dc, red);
        gdi.SetBackgroundMode(dc, BackgroundMode::Transparent);
        const int first = gdi.SaveDc(dc);
        if (first != 1) return false;
        gdi.SelectBrush(dc, secondBrush);
        gdi.SetTextColor(dc, blue);
        const int second = gdi.SaveDc(dc);
        gdi.SetTextColor(dc, OpaqueBlack);
        if (second != 2 || gdi.DeleteObject(firstBrush)) return false;
        if (gdi.RestoreDc(dc, 0) || gdi.RestoreDc(dc, -3)) return false;
        if (!gdi.RestoreDc(dc, -2)) return false;
        Color color = 0;
        Brush brush{};
        BackgroundMode mode{};
        if (!gdi.GetTextColor(dc, &color) || color != red || !gdi.GetSelectedBrush(dc, &brush) || brush.color != red ||
            !gdi.GetBackgroundMode(dc, &mode) || mode != BackgroundMode::Transparent) return false;
        if (gdi.RestoreDc(dc, first)) return false;
        const auto memory = gdi.CreateMemoryDc();
        const auto bitmap = gdi.CreateBitmap(3, 3, red);
        const auto other = gdi.CreateBitmap(2, 2, blue);
        gdi.SelectBitmap(memory, bitmap);
        if (gdi.SaveDc(memory) != 1) return false;
        gdi.SelectBitmap(memory, other);
        if (gdi.DeleteObject(bitmap) || !gdi.RestoreDc(memory, -1)) return false;
        const auto restored = gdi.GetSurface(memory);
        if (!restored || restored->Width() != 3 || *restored->PixelAt(0, 0) != red) return false;
        return gdi.DestroyDc(memory) && gdi.DeleteObject(bitmap);
    }
    bool ValidateRasterGraphicsFixtures()
    {
        using namespace Win32Bridge::Bridge;
        using namespace RasterGraphics;
        const auto black = MiniGdi::OpaqueBlack;
        const auto red = MiniGdi::MakeColor(255, 0, 0);
        const auto blue = MiniGdi::MakeColor(0, 0, 255);
        std::vector<MiniGdi::Color> pixels(16, MiniGdi::Transparent);
        const MiniGdi::Rect bounds{0, 0, 4, 4};
        const Vertex origin{0, 0, 0, 0, 0}, horizontal{4, 0, 0, 0, 0}, vertical{0, 4, 0, 0, 0};
        const Vertex end{4, 4, 65280, 0, 0};
        const auto all = [](int, int) { return true; };
        GradientRectangle(pixels.data(), 4, 4, origin, horizontal, vertical, end, true, bounds,
            [](int x, int y) { return x != 1 || y != 1; });
        if (pixels[0] != black || pixels[3] != MiniGdi::MakeColor(191, 0, 0) ||
            pixels[5] != MiniGdi::Transparent || pixels[15] != pixels[3]) return false;
        std::fill(pixels.begin(), pixels.end(), MiniGdi::Transparent);
        GradientRectangle(pixels.data(), 4, 4, origin, horizontal, vertical, end, false, bounds, all);
        if (pixels[3] != black || pixels[12] != MiniGdi::MakeColor(191, 0, 0)) return false;

        std::fill(pixels.begin(), pixels.end(), MiniGdi::Transparent);
        GradientTriangle(pixels.data(), 4, 4, {0, 0, 65280, 0, 0}, {4, 0, 0, 65280, 0},
            {0, 4, 0, 0, 65280}, bounds, all);
        if (pixels[0] != red || pixels[5] != MiniGdi::MakeColor(127, 63, 63) ||
            pixels[15] != MiniGdi::Transparent) return false;

        std::fill(pixels.begin(), pixels.end(), MiniGdi::Transparent);
        GradientRectangle(pixels.data(), 4, 4, {4, 0, 0, 0, 0}, {4, 4, 0, 0, 0}, {0, 0, 0, 0, 0},
            end, true, bounds, all);
        if (pixels[0] != black || pixels[12] != MiniGdi::MakeColor(191, 0, 0)) return false;
        unsigned visited = 0;
        GradientRectangle(pixels.data(), 4, 4, {-1.0e9, -1.0e9, 0, 0, 0}, {1.0e9, -1.0e9, 0, 0, 0},
            {-1.0e9, 1.0e9, 0, 0, 0}, end, true, bounds,
            [&](int, int) { ++visited; return true; });
        if (visited != 16) return false;
        const auto previous = pixels;
        GradientTriangle(pixels.data(), 4, 4, origin, origin, origin, bounds, all);
        if (pixels != previous) return false;

        if (AlphaOver(red, blue, 0, false) != blue || AlphaOver(red, blue, 255, false) != red) return false;
        if (AlphaOver(MiniGdi::MakeColor(128, 0, 0, 128), blue, 255, true) != MiniGdi::MakeColor(128, 0, 127))
            return false;
        std::fill(pixels.begin(), pixels.end(), blue);
        if (!Affine(pixels.data(), 4, 4, origin, horizontal, vertical, bounds,
            [](int x, int y) { return x < 2 && y < 2; },
            [&](double, double, MiniGdi::Color& pixel) { pixel = AlphaOver(red, pixel, 255, false); }, 0.5))
            return false;
        if (pixels[0] != red || pixels[5] != red || pixels[2] != blue || pixels[8] != blue) return false;
        if (!OverlapsRectangle(origin, horizontal, vertical, 1, 1, 2, 2) ||
            OverlapsRectangle(origin, horizontal, vertical, 4, 0, 8, 4)) return false;
        return !Affine(pixels.data(), 4, 4, origin, origin, origin, bounds, all,
            [](double, double, MiniGdi::Color&) {});
    }
    template<typename Procedure>
    Procedure AutomationProcedure(const wchar_t* name)
    {
        Win32Bridge::Bridge::ImportedSymbol symbol;
        symbol.library = L"oleaut32.dll";
        symbol.name = name;
        return reinterpret_cast<Procedure>(Win32Bridge::Bridge::ResolveAutomationDataImport(symbol).targetAddress);
    }

    bool ValidateAutomationFixtures()
    {
        using namespace Win32Bridge::Bridge;
        const auto create = AutomationProcedure<decltype(&::SafeArrayCreateVector)>(L"SafeArrayCreateVector");
        const auto put = AutomationProcedure<decltype(&::SafeArrayPutElement)>(L"SafeArrayPutElement");
        const auto copy = AutomationProcedure<decltype(&::SafeArrayCopy)>(L"SafeArrayCopy");
        const auto get = AutomationProcedure<decltype(&::SafeArrayGetElement)>(L"SafeArrayGetElement");
        const auto convert = AutomationProcedure<decltype(&::VariantChangeType)>(L"VariantChangeType");
        const auto copyIndirect = AutomationProcedure<decltype(&::VariantCopyInd)>(L"VariantCopyInd");
        if (!create || !put || !copy || !get || !convert || !copyIndirect) return false;
        ImportedSymbol ordinal;
        ordinal.library = L"OLEAUT32.DLL";
        ordinal.importedByOrdinal = true;
        ordinal.ordinal = 11;
        if (ResolveAutomationDataImport(ordinal).targetAddress != reinterpret_cast<ULONGLONG>(copyIndirect)) return false;
        for (const auto name : { L"LoadTypeLib", L"RegisterActiveObject", L"GetActiveObject", L"variantinit" })
            if (AutomationProcedure<void*>(name)) return false;

        struct Variant
        {
            VARIANT value;
            Variant() { ::VariantInit(&value); }
            ~Variant() { ::VariantClear(&value); }
        } source, converted, indirect, independent;
        V_VT(&source.value) = VT_BSTR;
        V_BSTR(&source.value) = ::SysAllocString(L"123");
        if (!V_BSTR(&source.value) || FAILED(convert(&converted.value, &source.value, 0, VT_I4)) ||
            V_VT(&converted.value) != VT_I4 || V_I4(&converted.value) != 123) return false;
        if (FAILED(convert(&converted.value, &source.value, 0, VT_UI1)) || V_UI1(&converted.value) != 123) return false;
        V_VT(&indirect.value) = VT_BSTR | VT_BYREF;
        V_BSTRREF(&indirect.value) = &V_BSTR(&source.value);
        if (FAILED(copyIndirect(&independent.value, &indirect.value)) ||
            V_VT(&independent.value) != VT_BSTR || V_BSTR(&independent.value) == V_BSTR(&source.value)) return false;
        if (FAILED(::VariantClear(&source.value)) || wcscmp(V_BSTR(&independent.value), L"123") != 0) return false;
        V_VT(&source.value) = VT_I4;
        V_I4(&source.value) = 256;
        if (convert(&converted.value, &source.value, 0, VT_UI1) != DISP_E_OVERFLOW) return false;

        const auto destroy = [](SAFEARRAY* array) { if (array) ::SafeArrayDestroy(array); };
        using Array = std::unique_ptr<SAFEARRAY, decltype(destroy)>;
        Array strings(create(VT_BSTR, -2, 2), destroy);
        if (!strings) return false;
        LONG index = -2;
        BSTR text = ::SysAllocString(L"array contents");
        if (!text) return false;
        const HRESULT stored = put(strings.get(), &index, text);
        ::SysFreeString(text);
        if (FAILED(stored)) return false;
        LONG lower = 0, upper = 0;
        if (FAILED(::SafeArrayGetLBound(strings.get(), 1, &lower)) || lower != -2 ||
            FAILED(::SafeArrayGetUBound(strings.get(), 1, &upper)) || upper != -1) return false;
        SAFEARRAY* rawCopy = nullptr;
        if (FAILED(copy(strings.get(), &rawCopy))) return false;
        Array copied(rawCopy, destroy);
        strings.reset();
        BSTR extracted = nullptr;
        if (FAILED(get(copied.get(), &index, &extracted)) || !extracted) return false;
        const bool equal = wcscmp(extracted, L"array contents") == 0;
        ::SysFreeString(extracted);
        if (!equal) return false;
        void* data = nullptr;
        if (FAILED(::SafeArrayAccessData(copied.get(), &data))) return false;
        const HRESULT locked = ::SafeArrayDestroy(copied.get());
        if (FAILED(::SafeArrayUnaccessData(copied.get())) || locked != DISP_E_ARRAYISLOCKED) return false;

        class ReferenceProbe final : public IUnknown
        {
        public:
            ULONG references = 1;
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
            {
                if (!result) return E_POINTER;
                *result = nullptr;
                if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
                *result = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
            ULONG STDMETHODCALLTYPE Release() override { return --references; }
        } probe;
        Array objects(create(VT_UNKNOWN, 0, 1), destroy);
        index = 0;
        if (!objects || FAILED(put(objects.get(), &index, static_cast<IUnknown*>(&probe))) || probe.references != 2)
            return false;
        rawCopy = nullptr;
        if (FAILED(copy(objects.get(), &rawCopy))) return false;
        Array objectCopy(rawCopy, destroy);
        if (probe.references != 3) return false;
        objects.reset();
        objectCopy.reset();
        return probe.references == 1;
    }
    bool ValidateComIdentifierFixtures()
    {
        using namespace Win32Bridge::Bridge;
        GUID guid{};
        if (!ParseGuestGuid(L"{01234567-89ab-CDEF-8012-3456789abcde}", &guid) ||
            guid.Data1 != 0x01234567 || guid.Data2 != 0x89ab || guid.Data3 != 0xcdef ||
            guid.Data4[0] != 0x80 || guid.Data4[1] != 0x12 || guid.Data4[7] != 0xde) return false;
        wchar_t buffer[39]{};
        if (FormatGuestGuid(guid, buffer, 39) != 39 ||
            wcscmp(buffer, L"{01234567-89AB-CDEF-8012-3456789ABCDE}") != 0) return false;
        GUID roundTrip{};
        if (!ParseGuestGuid(buffer, &roundTrip) || !IsEqualGUID(guid, roundTrip)) return false;
        buffer[0] = L'!';
        if (FormatGuestGuid(guid, buffer, 38) != 0 || buffer[0] != L'!') return false;
        const wchar_t* invalid[] = { L"", L"01234567-89AB-CDEF-8012-3456789ABCDE",
            L"{01234567-89AB-CDEF-8012-3456789ABCDE}extra",
            L"{01234567_89AB-CDEF-8012-3456789ABCDE}",
            L"{01234567-89AB-CDEF-8012-3456789ABCDZ}" };
        for (const auto text : invalid)
        {
            roundTrip = guid;
            if (ParseGuestGuid(text, &roundTrip) || !IsEqualGUID(guid, roundTrip)) return false;
        }
        return !ParseGuestGuid(nullptr, &guid) && !ParseGuestGuid(buffer, nullptr) &&
            FormatGuestGuid(guid, nullptr, 39) == 0;
    }
    bool ValidateMemoryStreamFixtures()
    {
        using namespace Win32Bridge::Bridge;
        const auto release = [](IStream* stream) { if (stream) stream->Release(); };
        using Stream = std::unique_ptr<IStream, decltype(release)>;
        const BYTE initial[] = { 1, 2, 3 };
        Stream stream(BridgeSHCreateMemStream(initial, sizeof(initial)), release);
        if (!stream || BridgeSHCreateMemStream(nullptr, 1)) return false;
        BYTE buffer[8]{};
        ULONG count = 99;
        if (stream->Read(buffer, 4, &count) != S_FALSE || count != 3 ||
            std::memcmp(buffer, initial, 3) != 0) return false;
        if (FAILED(BridgeIStreamReset(stream.get())) ||
            BridgeIStreamRead(stream.get(), buffer, 4) != E_FAIL) return false;

        IStream* rawClone = nullptr;
        if (FAILED(stream->Clone(&rawClone))) return false;
        Stream clone(rawClone, release);
        if (FAILED(BridgeIStreamReset(clone.get()))) return false;
        const BYTE changed = 9;
        if (FAILED(BridgeIStreamWrite(clone.get(), &changed, 1))) return false;
        if (stream->Read(buffer, 1, &count) != S_FALSE || count != 0) return false;
        if (FAILED(BridgeIStreamReset(stream.get())) ||
            FAILED(BridgeIStreamRead(stream.get(), buffer, 1)) || buffer[0] != changed) return false;

        LARGE_INTEGER move{};
        move.QuadPart = 6;
        if (FAILED(stream->Seek(move, STREAM_SEEK_SET, nullptr)) ||
            FAILED(BridgeIStreamWrite(stream.get(), &changed, 1))) return false;
        ULARGE_INTEGER size{};
        if (FAILED(BridgeIStreamSize(stream.get(), &size)) || size.QuadPart != 7) return false;
        move.QuadPart = 3;
        if (FAILED(stream->Seek(move, STREAM_SEEK_SET, nullptr)) ||
            FAILED(BridgeIStreamRead(stream.get(), buffer, 4)) ||
            buffer[0] || buffer[1] || buffer[2] || buffer[3] != changed) return false;

        move.QuadPart = 0x100000001LL;
        ULARGE_INTEGER position{};
        if (FAILED(stream->Seek(move, STREAM_SEEK_SET, &position)) ||
            position.QuadPart != 0x100000001ULL) return false;
        size.QuadPart = 2;
        if (FAILED(stream->SetSize(size)) || FAILED(BridgeIStreamSize(clone.get(), &size)) ||
            size.QuadPart != 2) return false;
        move.QuadPart = 0;
        if (FAILED(stream->Seek(move, STREAM_SEEK_CUR, &position)) ||
            position.QuadPart != 0x100000001ULL) return false;
        move.QuadPart = -1;
        if (stream->Seek(move, STREAM_SEEK_SET, nullptr) != STG_E_INVALIDFUNCTION) return false;
        move.QuadPart = 0;
        if (FAILED(stream->Seek(move, STREAM_SEEK_CUR, &position)) ||
            position.QuadPart != 0x100000001ULL) return false;

        Stream destination(BridgeSHCreateMemStream(nullptr, 0), release);
        if (!destination || FAILED(BridgeIStreamReset(stream.get()))) return false;
        ULARGE_INTEGER read{}, written{}, requested{};
        requested.QuadPart = 99;
        if (stream->CopyTo(destination.get(), requested, &read, &written) != S_FALSE ||
            read.QuadPart != 2 || written.QuadPart != 2) return false;
        if (FAILED(BridgeIStreamReset(stream.get())) ||
            stream->CopyTo(stream.get(), requested, &read, &written) != S_FALSE ||
            read.QuadPart != 2 || written.QuadPart != 2) return false;
        void* unknown = nullptr;
        if (FAILED(stream->QueryInterface(__uuidof(IUnknown), &unknown)) || !unknown) return false;
        static_cast<IUnknown*>(unknown)->Release();
        ImportedSymbol symbol;
        symbol.library = L"shcore.dll";
        symbol.name = L"SHCreateMemStream";
        return ResolveMemoryStreamImport(symbol).targetAddress ==
            reinterpret_cast<ULONGLONG>(&BridgeSHCreateMemStream);
    }
    bool ValidateExportAliasFixtures()
    {
        using namespace Win32Bridge::Bridge;
        struct Fixture
        {
            const wchar_t* library;
            const wchar_t* name;
            WORD ordinal;
            const wchar_t* targetLibrary;
            const wchar_t* targetName;
        };
        const Fixture fixtures[] = {
            { L"COMCTL32.DLL", L"", 17, L"comctl32.dll", L"InitCommonControls" },
            { L"shlwapi.dll", L"", 36, L"user32.dll", L"AppendMenuW" },
            { L"version.dll", L"VerLanguageNameW", 0, L"kernel32.dll", L"VerLanguageNameW" },
            { L"ucrtbase.dll", L"_o_malloc", 0, L"ucrtbase.dll", L"malloc" },
            { L"UCRTBASE.DLL", L"_o__dup2", 0, L"UCRTBASE.DLL", L"_dup2" },
            { L"ucrtbase.dll", L"_o___stdio_common_vfprintf", 0, L"ucrtbase.dll", L"__stdio_common_vfprintf" }
        };
        for (const auto& fixture : fixtures)
        {
            ImportedSymbol source;
            source.library = fixture.library;
            source.name = fixture.name;
            source.ordinal = fixture.ordinal;
            source.importedByOrdinal = fixture.ordinal != 0;
            source.iatRva = 0x1234;
            source.delayLoaded = true;
            ImportedSymbol target;
            if (!ExportAliases::Redirect(source, &target) ||
                target.library != fixture.targetLibrary || target.name != fixture.targetName ||
                target.importedByOrdinal || target.ordinal != 0 ||
                target.iatRva != source.iatRva || target.delayLoaded != source.delayLoaded)
            {
                std::wcerr << L"Export alias fixture failed for " << fixture.library << L"!"
                    << fixture.name << L" ordinal " << fixture.ordinal << L"\n";
                return false;
            }
        }
        ImportedSymbol source;
        ImportedSymbol target;
        source.library = L"version.dll";
        source.name = L"verLanguageNameW";
        if (ExportAliases::Redirect(source, &target)) return false;
        source.name = L"NoSuchExport";
        if (ExportAliases::Redirect(source, &target)) return false;
        source.library = L"user32.dll"; source.name = L"_o_malloc";
        if (ExportAliases::Redirect(source, &target)) return false;
        source.library = L"ucrtbase.dll"; source.name = L"_o__o_malloc";
        if (ExportAliases::Redirect(source, &target)) return false;
        source.name = L"_o_";
        if (ExportAliases::Redirect(source, &target)) return false;
        source.library = L"comctl32.dll";
        source.name = L"#17";
        if (ExportAliases::Redirect(source, &target)) return false;
        source.importedByOrdinal = true;
        source.ordinal = 0xffff;
        return !ExportAliases::Redirect(source, &target) && !ExportAliases::Redirect(source, nullptr);
    }
    // These cases intentionally do not depend on the embedded demo's current
    // import table. They keep the catalog contract visible as the bridge gains
    // adapters before a new guest fixture has to be compiled and staged.
    bool ValidateResolverFixtures()
    {
        using Win32Bridge::Bridge::CompatibilityCatalog;
        using Win32Bridge::Bridge::ImportDisposition;
        using Win32Bridge::Bridge::ImportedSymbol;

        const ResolverFixture fixtures[] =
        {
            { L"USER32 window creation", L"USER32.DLL", L"CreateWindowExW", ImportDisposition::NeedsBridge },
            { L"USER32 cursor bootstrap", L"user32.dll", L"LoadCursorW", ImportDisposition::NeedsBridge },
            { L"USER32 virtual desktop metrics", L"api-ms-win-ntuser-window-l1-1-0.dll", L"GetSystemMetrics", ImportDisposition::NeedsBridge },
            { L"USER32 virtual system brush", L"user32.dll", L"GetSysColorBrush", ImportDisposition::NeedsBridge },
            { L"USER32 client-only frame adjustment", L"user32.dll", L"AdjustWindowRectEx", ImportDisposition::NeedsBridge },
            { L"USER32 dialog control lookup", L"user32.dll", L"GetDlgItem", ImportDisposition::NeedsBridge },
            { L"USER32 dialog control text through API set", L"api-ms-win-ntuser-window-l1-1-0.dll", L"SetDlgItemTextW", ImportDisposition::NeedsBridge },
            { L"USER32 message dispatch through API set", L"api-ms-win-ntuser-message-l1-1-0.dll", L"DispatchMessageW", ImportDisposition::NeedsBridge },
            { L"USER32 paint cycle", L"ext-ms-win-ntuser-draw-l1-1-0.dll", L"BeginPaint", ImportDisposition::NeedsBridge },
            { L"USER32 SetTimer", L"user32.dll", L"SetTimer", ImportDisposition::NeedsBridge },
            { L"USER32 KillTimer through API set", L"api-ms-win-ntuser-window-l1-1-0.dll", L"KillTimer", ImportDisposition::NeedsBridge },
            { L"MiniGDI primitive", L"GDI32.dll", L"Ellipse", ImportDisposition::NeedsBridge },
            { L"MiniGDI device capability", L"gdi32.dll", L"GetDeviceCaps", ImportDisposition::NeedsBridge },
            { L"MiniGDI CreateFontW", L"gdi32full.dll", L"CreateFontW", ImportDisposition::NeedsBridge },
            { L"MiniGDI CreateFontIndirectW through API set", L"api-ms-win-gdi-draw-l1-1-0.dll", L"CreateFontIndirectW", ImportDisposition::NeedsBridge },
            { L"kernel module handle", L"KERNELBASE.dll", L"GetModuleHandleW", ImportDisposition::NeedsBridge },
            { L"kernel tick count through API set", L"api-ms-win-core-sysinfo-l1-2-0.dll", L"GetTickCount", ImportDisposition::NeedsBridge },
            { L"kernel performance counter", L"kernel32.dll", L"QueryPerformanceCounter", ImportDisposition::NeedsBridge },
        };

        bool passed = true;
        for (const auto& fixture : fixtures)
        {
            ImportedSymbol symbol{};
            symbol.library = fixture.library;
            symbol.name = fixture.name;

            const auto resolution = CompatibilityCatalog::Resolve(symbol);
            if (resolution.disposition != fixture.expectedDisposition)
            {
                std::wcerr << L"Resolver fixture failed: " << fixture.description
                    << L" (" << fixture.library << L"!" << fixture.name << L")\n";
                passed = false;
            }
        }

        ImportedSymbol ordinal{};
        ordinal.library = L"user32.dll";
        ordinal.ordinal = 1;
        ordinal.importedByOrdinal = true;
        if (CompatibilityCatalog::Resolve(ordinal).disposition != ImportDisposition::Unsupported)
        {
            std::wcerr << L"Resolver fixture failed: ordinal imports must remain explicit.\n";
            passed = false;
        }

        return passed;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"Usage: BridgeTests <guest-pe-path>\n";
        return 2;
    }

    if (!ValidateCrtMultibyteFixtures() || !ValidateCrtLocaleFixtures() || !ValidateCrtThreadStateFixtures() || !ValidateCrtHeapAdapterFixtures() || !ValidateCrtRttiFixtures() || !ValidateX64UnwindFixtures() || !ValidateCrtExceptionPolicyFixtures() || !ValidateCrtExceptionIsolationFixtures() || !ValidateCrtExceptionPtrFixtures() || !ValidateCrtHeapFixtures() || !ValidateConsoleBufferFixtures() || !ValidateThreadpoolTimerFixtures() || !ValidateCrtStreamFixtures() || !ValidatePipeFixtures() || !ValidateConsoleFixtures() || !ValidateCrtExceptionImageFixtures() || !ValidateCrtExceptionDispatchFixtures() || !ValidateCrtExceptionThreadStateFixtures() || !ValidateCrtExceptionBoundaryFixtures() || !ValidateCrtScanningFixtures() || !ValidateCrtStreamScanningFixtures() || !ValidateCrtScannedFileFixtures() || !ValidateCrtFormattingFixtures() || !ValidateCrtFormattedStreamFixtures() || !ValidateCrtStdioModeFixtures() || !ValidateCrtStdioFixtures() || !ValidateCrtWideStdioFixtures() || !ValidateCrtUnicodeFileFixtures() || !ValidateCrtFileFixtures() || !ValidateProcessEnvironmentFixtures() || !ValidateCrtProcessFixtures() || !ValidateCrtDataFixtures() || !ValidateDcStateFixtures() ||
        !ValidateRasterGraphicsFixtures() || !ValidateAutomationFixtures() ||
        !ValidateComIdentifierFixtures() || !ValidateMemoryStreamFixtures() ||
        !ValidateExportAliasFixtures() || !ValidateResolverFixtures())
    {
        return 1;
    }

    std::ifstream stream(argv[1], std::ios::binary);
    const std::vector<BYTE> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());

    Win32Bridge::Bridge::PeImageInfo image;
    if (!Win32Bridge::Bridge::PeImage::Inspect(bytes.data(), bytes.size(), &image))
    {
        std::wcerr << L"PE inspection failed: " << image.error << L"\n";
        return 1;
    }

    const auto hasImport = [&image](const wchar_t* library, const wchar_t* name)
    {
        for (const auto& symbol : image.imports)
        {
            if (symbol.library == library && symbol.name == name)
            {
                return true;
            }
        }
        return false;
    };

    const bool hasStorageRoundTrip =
        hasImport(L"KERNEL32.dll", L"CreateFileW") &&
        hasImport(L"KERNEL32.dll", L"ReadFile") &&
        hasImport(L"KERNEL32.dll", L"WriteFile") &&
        hasImport(L"KERNEL32.dll", L"CloseHandle") &&
        hasImport(L"KERNEL32.dll", L"CreateDirectoryW");

    const bool hasWindowAndMessageLoop =
        hasImport(L"USER32.dll", L"RegisterClassExW") &&
        hasImport(L"USER32.dll", L"CreateWindowExW") &&
        hasImport(L"USER32.dll", L"ShowWindow") &&
        hasImport(L"USER32.dll", L"UpdateWindow") &&
        hasImport(L"USER32.dll", L"GetMessageW") &&
        hasImport(L"USER32.dll", L"TranslateMessage") &&
        hasImport(L"USER32.dll", L"DispatchMessageW") &&
        hasImport(L"USER32.dll", L"PostQuitMessage") &&
        hasImport(L"USER32.dll", L"DestroyWindow") &&
        hasImport(L"USER32.dll", L"DefWindowProcW");

    const bool hasPaintCycle =
        hasImport(L"USER32.dll", L"BeginPaint") &&
        hasImport(L"USER32.dll", L"EndPaint") &&
        hasImport(L"USER32.dll", L"GetClientRect") &&
        hasImport(L"USER32.dll", L"FillRect");

    const bool hasMiniGdiDrawing =
        hasImport(L"GDI32.dll", L"CreateSolidBrush") &&
        hasImport(L"GDI32.dll", L"CreatePen") &&
        hasImport(L"GDI32.dll", L"SelectObject") &&
        hasImport(L"GDI32.dll", L"DeleteObject") &&
        hasImport(L"GDI32.dll", L"Rectangle") &&
        hasImport(L"GDI32.dll", L"MoveToEx") &&
        hasImport(L"GDI32.dll", L"LineTo") &&
        hasImport(L"GDI32.dll", L"SetBkMode") &&
        hasImport(L"GDI32.dll", L"SetTextColor") &&
        hasImport(L"GDI32.dll", L"TextOutW");

    if (image.machine != IMAGE_FILE_MACHINE_AMD64)
    {
        std::wcerr << L"Guest PE must be AMD64.\n";
        return 1;
    }
    if (!hasStorageRoundTrip)
    {
        std::wcerr << L"Guest PE is missing the virtual-storage round-trip imports.\n";
        return 1;
    }
    if (!hasWindowAndMessageLoop)
    {
        std::wcerr << L"Guest PE is missing the USER32 window/message-loop imports.\n";
        return 1;
    }
    if (!hasPaintCycle)
    {
        std::wcerr << L"Guest PE is missing the USER32 paint-cycle imports.\n";
        return 1;
    }
    if (!hasMiniGdiDrawing)
    {
        std::wcerr << L"Guest PE is missing the MiniGDI drawing imports.\n";
        return 1;
    }

    Win32Bridge::Bridge::MappedPeImage mappedImage;
    std::wstring mappingError;
    if (!Win32Bridge::Bridge::PeMapper::Materialize(bytes.data(), bytes.size(), &mappedImage, &mappingError) ||
        !Win32Bridge::Bridge::PeMapper::ApplyBaseRelocations(&mappedImage, mappedImage.preferredImageBase, &mappingError))
    {
        std::wcerr << L"PE mapping failed: " << mappingError << L"\n";
        return 1;
    }

    Win32Bridge::Bridge::BindingReport binding;
    std::wstring bindingError;
    const auto resolver = [](const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        auto resolution = Win32Bridge::Bridge::CompatibilityCatalog::Resolve(symbol);
        if (resolution.disposition == Win32Bridge::Bridge::ImportDisposition::NeedsBridge)
        {
            // The native test intentionally uses synthetic addresses; the UWP
            // host supplies the actual ABI adapters at runtime.
            resolution.targetAddress = 0x1234567800000000ULL + symbol.iatRva;
        }
        return resolution;
    };
    if (!Win32Bridge::Bridge::ImportBinder::Bind(&mappedImage, image, resolver, &binding, &bindingError) ||
        binding.bound != image.imports.size() || binding.unresolved != 0)
    {
        std::wcerr << L"IAT binding failed: " << bindingError << L"\n";
        return 1;
    }

    bool everyImportWasBound = true;
    for (const auto& symbol : image.imports)
    {
        ULONGLONG address = 0;
        memcpy(&address, mappedImage.bytes.data() + symbol.iatRva, sizeof(address));
        everyImportWasBound = everyImportWasBound && address != 0;
    }
    if (!everyImportWasBound)
    {
        std::wcerr << L"At least one test IAT target was not written.\n";
        return 1;
    }

    std::wcout << L"Validated, materialized, and fully bound x64 PE with " << image.imports.size() << L" imports.\n";
    return 0;
}
