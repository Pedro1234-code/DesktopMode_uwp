#include "pch.h"
#include "Bridge/DialogResources.h"

#include "Bridge/GuestResources.h"
#include "Bridge/GuestWindow.h"
#include "Bridge/Kernel32Shims.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr WORD DialogResourceType = 5; // RT_DIALOG
    constexpr DWORD DialogSetFont = 0x00000040;
    constexpr DWORD DialogChildStyle = 0x40000000;
    constexpr DWORD DialogVisibleStyle = 0x10000000;

    struct DialogValue final
    {
        bool ordinal = false;
        WORD id = 0;
        std::wstring text;
    };

    class Reader final
    {
    public:
        Reader(const BYTE* begin, const BYTE* end) : m_current(begin), m_end(end) {}

        bool Word(WORD* value) { return Read(value); }
        bool Dword(DWORD* value) { return Read(value); }
        bool Short(short* value) { return Read(value); }

        bool AlignDword()
        {
            const uintptr_t address = reinterpret_cast<uintptr_t>(m_current);
            const uintptr_t aligned = (address + 3u) & ~uintptr_t(3u);
            if (aligned < address || aligned > reinterpret_cast<uintptr_t>(m_end)) return false;
            m_current = reinterpret_cast<const BYTE*>(aligned);
            return true;
        }

        bool Value(DialogValue* value)
        {
            if (!value) return false;
            *value = DialogValue{};
            WORD first = 0;
            if (!Word(&first)) return false;
            if (first == 0) return true;
            if (first == 0xffff)
            {
                value->ordinal = true;
                return Word(&value->id);
            }
            value->text.push_back(static_cast<wchar_t>(first));
            for (;;)
            {
                WORD character = 0;
                if (!Word(&character)) return false;
                if (character == 0) return true;
                value->text.push_back(static_cast<wchar_t>(character));
            }
        }

        bool Skip(size_t count)
        {
            if (count > static_cast<size_t>(m_end - m_current)) return false;
            m_current += count;
            return true;
        }

    private:
        template <typename T>
        bool Read(T* value)
        {
            if (!value || sizeof(T) > static_cast<size_t>(m_end - m_current)) return false;
            std::memcpy(value, m_current, sizeof(T));
            m_current += sizeof(T);
            return true;
        }

        const BYTE* m_current;
        const BYTE* m_end;
    };

    struct DialogItem final
    {
        DWORD style = 0;
        DWORD extendedStyle = 0;
        short x = 0;
        short y = 0;
        short width = 0;
        short height = 0;
        DWORD id = 0;
        DialogValue windowClass;
        DialogValue title;
    };

    struct DialogTemplate final
    {
        DWORD style = 0;
        DWORD extendedStyle = 0;
        short x = 0;
        short y = 0;
        short width = 0;
        short height = 0;
        DialogValue title;
        std::vector<DialogItem> items;
    };

    struct ModalDialogState final
    {
        ModalDialogState* previous = nullptr;
        HWND window = nullptr;
        INT_PTR result = -1;
        bool ended = false;
    };

    thread_local ModalDialogState* g_activeModalDialog = nullptr;
    std::atomic<unsigned long> g_nextDialogClass{ 1 };

    bool ImageRange(const BYTE* address, size_t count)
    {
        const BYTE* image = CurrentGuestImageBase();
        const size_t imageSize = CurrentGuestImageSize();
        return image && address >= image && count <= imageSize && static_cast<size_t>(address - image) <= imageSize - count;
    }

    const IMAGE_RESOURCE_DIRECTORY_ENTRY* FindResourceEntry(
        const BYTE* resourceBase,
        const IMAGE_RESOURCE_DIRECTORY* directory,
        const DialogValue& requested)
    {
        if (!resourceBase || !directory || !ImageRange(reinterpret_cast<const BYTE*>(directory), sizeof(*directory))) return nullptr;
        const size_t count = static_cast<size_t>(directory->NumberOfNamedEntries) + directory->NumberOfIdEntries;
        const auto entries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(directory + 1);
        if (!ImageRange(reinterpret_cast<const BYTE*>(entries), count * sizeof(*entries))) return nullptr;
        for (size_t index = 0; index < count; ++index)
        {
            const auto& entry = entries[index];
            if (requested.ordinal)
            {
                if (!entry.NameIsString && entry.Id == requested.id) return &entry;
                continue;
            }
            if (!entry.NameIsString) continue;
            const auto string = reinterpret_cast<const IMAGE_RESOURCE_DIR_STRING_U*>(resourceBase + entry.NameOffset);
            if (!ImageRange(reinterpret_cast<const BYTE*>(string), sizeof(WORD))) continue;
            const size_t bytes = sizeof(WORD) + static_cast<size_t>(string->Length) * sizeof(WCHAR);
            if (!ImageRange(reinterpret_cast<const BYTE*>(string), bytes)) continue;
            if (requested.text.size() == string->Length &&
                std::wmemcmp(requested.text.data(), string->NameString, string->Length) == 0) return &entry;
        }
        return nullptr;
    }

    const IMAGE_RESOURCE_DIRECTORY* ChildDirectory(const BYTE* resourceBase, const IMAGE_RESOURCE_DIRECTORY_ENTRY* entry)
    {
        if (!entry || !entry->DataIsDirectory) return nullptr;
        const auto directory = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(resourceBase + entry->OffsetToDirectory);
        return ImageRange(reinterpret_cast<const BYTE*>(directory), sizeof(*directory)) ? directory : nullptr;
    }

    bool FindTypedResource(WORD resourceType, const DialogValue& requested, const BYTE** data, size_t* size)
    {
        if (!data || !size) return false;
        *data = nullptr;
        *size = 0;
        const BYTE* image = CurrentGuestImageBase();
        if (!image || CurrentGuestImageSize() < sizeof(IMAGE_DOS_HEADER)) return false;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || !ImageRange(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_RESOURCE_DIRECTORY) ||
            !ImageRange(image + directory.VirtualAddress, directory.Size)) return false;

        const BYTE* resourceBase = image + directory.VirtualAddress;
        const auto root = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(resourceBase);
        DialogValue type;
        type.ordinal = true;
        type.id = resourceType;
        const auto typeEntry = FindResourceEntry(resourceBase, root, type);
        const auto names = ChildDirectory(resourceBase, typeEntry);
        const auto nameEntry = FindResourceEntry(resourceBase, names, requested);
        const auto languages = ChildDirectory(resourceBase, nameEntry);
        if (!languages) return false;
        const size_t languageCount = static_cast<size_t>(languages->NumberOfNamedEntries) + languages->NumberOfIdEntries;
        const auto languageEntries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(languages + 1);
        if (languageCount == 0 || !ImageRange(reinterpret_cast<const BYTE*>(languageEntries), languageCount * sizeof(*languageEntries))) return false;
        const auto dataEntry = reinterpret_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(resourceBase + languageEntries[0].OffsetToData);
        if (languageEntries[0].DataIsDirectory || !ImageRange(reinterpret_cast<const BYTE*>(dataEntry), sizeof(*dataEntry)) ||
            !ImageRange(image + dataEntry->OffsetToData, dataEntry->Size)) return false;
        *data = image + dataEntry->OffsetToData;
        *size = dataEntry->Size;
        return true;
    }

    bool SkipDialogFont(Reader* reader, bool extended)
    {
        WORD pointSize = 0;
        if (!reader->Word(&pointSize)) return false;
        if (extended)
        {
            WORD weight = 0;
            WORD italicAndCharset = 0;
            if (!reader->Word(&weight) || !reader->Word(&italicAndCharset)) return false;
        }
        DialogValue face;
        return reader->Value(&face);
    }

    bool ParseDialog(const BYTE* data, size_t size, DialogTemplate* dialog)
    {
        if (!data || !dialog || size < 18) return false;
        *dialog = DialogTemplate{};
        Reader reader(data, data + size);
        WORD first = 0;
        WORD second = 0;
        if (!reader.Word(&first) || !reader.Word(&second)) return false;
        const bool extended = first == 1 && second == 0xffff;
        WORD itemCount = 0;
        if (extended)
        {
            DWORD ignored = 0;
            if (!reader.Dword(&ignored) || !reader.Dword(&dialog->extendedStyle) || !reader.Dword(&dialog->style) ||
                !reader.Word(&itemCount) || !reader.Short(&dialog->x) || !reader.Short(&dialog->y) ||
                !reader.Short(&dialog->width) || !reader.Short(&dialog->height)) return false;
        }
        else
        {
            dialog->style = static_cast<DWORD>(first) | (static_cast<DWORD>(second) << 16);
            if (!reader.Dword(&dialog->extendedStyle) || !reader.Word(&itemCount) || !reader.Short(&dialog->x) ||
                !reader.Short(&dialog->y) || !reader.Short(&dialog->width) || !reader.Short(&dialog->height)) return false;
        }
        DialogValue ignored;
        if (!reader.Value(&ignored) || !reader.Value(&ignored) || !reader.Value(&dialog->title)) return false;
        if ((dialog->style & DialogSetFont) && !SkipDialogFont(&reader, extended)) return false;

        dialog->items.reserve(itemCount);
        for (WORD index = 0; index < itemCount; ++index)
        {
            if (!reader.AlignDword()) return false;
            DialogItem item;
            if (extended)
            {
                DWORD ignored = 0;
                if (!reader.Dword(&ignored) || !reader.Dword(&item.extendedStyle) || !reader.Dword(&item.style) ||
                    !reader.Short(&item.x) || !reader.Short(&item.y) || !reader.Short(&item.width) || !reader.Short(&item.height) ||
                    !reader.Dword(&item.id)) return false;
            }
            else
            {
                WORD id = 0;
                if (!reader.Dword(&item.style) || !reader.Dword(&item.extendedStyle) || !reader.Short(&item.x) ||
                    !reader.Short(&item.y) || !reader.Short(&item.width) || !reader.Short(&item.height) || !reader.Word(&id)) return false;
                item.id = id;
            }
            WORD creationDataSize = 0;
            if (!reader.Value(&item.windowClass) || !reader.Value(&item.title) || !reader.Word(&creationDataSize) ||
                !reader.Skip(creationDataSize)) return false;
            dialog->items.push_back(std::move(item));
        }
        return true;
    }

    LPCWSTR ClassNameFor(const DialogValue& value)
    {
        if (!value.ordinal) return value.text.empty() ? L"static" : value.text.c_str();
        switch (value.id)
        {
        case 0x0080: return L"button";
        case 0x0081: return L"edit";
        case 0x0082: return L"static";
        case 0x0083: return L"listbox";
        case 0x0084: return L"scrollbar";
        case 0x0085: return L"combobox";
        default: return L"static";
        }
    }

    int PixelsX(short units) { return static_cast<int>(units) * 2; }
    int PixelsY(short units) { return static_cast<int>(units) * 2; }

    // A DLGPROC is not a WNDPROC: returning FALSE means "not handled" for a
    // dialog message, while returning FALSE from WM_NCCREATE tells USER32 to
    // abort window creation. Use this tiny WNDPROC only for the creation
    // handshake, then install the guest DLGPROC once the HWND exists.
    LRESULT CALLBACK DialogCreationProcedure(HWND, UINT message, WPARAM, LPARAM)
    {
        return message == WM_NCCREATE ? TRUE : 0;
    }
}

INT_PTR Win32Bridge::Bridge::ShowGuestDialogFromResource(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure, LPARAM initParameter)
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!manager || !templateName)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    DialogValue requested;
    if (reinterpret_cast<ULONG_PTR>(templateName) <= 0xffff)
    {
        requested.ordinal = true;
        requested.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(templateName));
    }
    else
    {
        requested.text = templateName;
    }

    const BYTE* bytes = nullptr;
    size_t byteCount = 0;
    DialogTemplate dialog;
    if (!FindTypedResource(DialogResourceType, requested, &bytes, &byteCount) || !ParseDialog(bytes, byteCount, &dialog))
    {
        BridgeSetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return -1;
    }

    const std::wstring className = L"Win32Bridge.Dialog." + std::to_wstring(g_nextDialogClass.fetch_add(1));
    GuestAbi::WndClassW windowClass{};
    windowClass.lpfnWndProc = &DialogCreationProcedure;
    windowClass.hInstance = instance;
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(COLOR_BTNFACE + 1));
    windowClass.lpszClassName = className.c_str();
    DWORD error = ERROR_SUCCESS;
    if (manager->RegisterGuestClass(&windowClass, &error) == 0)
    {
        BridgeSetLastError(error);
        return -1;
    }

    const std::wstring title = dialog.title.ordinal ? L"" : dialog.title.text;
    HWND root = manager->CreateGuestWindow(dialog.extendedStyle, className.c_str(), title.c_str(), dialog.style | DialogVisibleStyle,
        PixelsX(dialog.x), PixelsY(dialog.y), PixelsX(dialog.width), PixelsY(dialog.height), parent, nullptr, instance, nullptr, &error);
    if (!root)
    {
        BridgeSetLastError(error);
        return -1;
    }
    if (procedure)
    {
        manager->SetGuestWindowLongPtr(
            root,
            GuestAbi::GwlpWndProc,
            reinterpret_cast<LONG_PTR>(procedure),
            &error);
        if (error != ERROR_SUCCESS)
        {
            manager->DestroyGuestWindow(root, nullptr);
            BridgeSetLastError(error);
            return -1;
        }
    }
    for (const auto& item : dialog.items)
    {
        const std::wstring itemTitle = item.title.ordinal ? L"" : item.title.text;
        manager->CreateGuestWindow(item.extendedStyle, ClassNameFor(item.windowClass), itemTitle.c_str(), item.style | DialogChildStyle | DialogVisibleStyle,
            PixelsX(item.x), PixelsY(item.y), PixelsX(item.width), PixelsY(item.height), root,
            reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(item.id)), instance, nullptr, &error);
    }

    ModalDialogState state;
    state.previous = g_activeModalDialog;
    state.window = root;
    g_activeModalDialog = &state;
    if (procedure) procedure(root, WM_INITDIALOG, 0, initParameter);
    manager->ShowGuestWindow(root, SW_SHOW, &error);
    while (!state.ended && manager->IsGuestWindow(root))
    {
        GuestAbi::Message message{};
        if (manager->GetGuestMessage(&message, nullptr, 0, 0) != GuestGetMessageResult::Message)
        {
            state.result = -1;
            break;
        }
        manager->TranslateGuestMessage(&message);
        manager->DispatchGuestMessage(&message, &error);
    }
    if (manager->IsGuestWindow(root)) manager->DestroyGuestWindow(root, &error);
    g_activeModalDialog = state.previous;
    BridgeSetLastError(ERROR_SUCCESS);
    return state.result;
}

BOOL Win32Bridge::Bridge::EndGuestResourceDialog(HWND dialog, INT_PTR result)
{
    for (ModalDialogState* state = g_activeModalDialog; state; state = state->previous)
    {
        if (state->window != dialog) continue;
        state->result = result;
        state->ended = true;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        DWORD error = ERROR_SUCCESS;
        if (!manager || !manager->DestroyGuestWindow(dialog, &error))
        {
            BridgeSetLastError(error);
            return FALSE;
        }
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}

int Win32Bridge::Bridge::LoadGuestStringResource(UINT identifier, LPWSTR buffer, int bufferCount)
{
    if (buffer && bufferCount > 0) buffer[0] = L'\0';
    DialogValue group;
    group.ordinal = true;
    group.id = static_cast<WORD>((identifier >> 4) + 1);
    const BYTE* bytes = nullptr;
    size_t byteCount = 0;
    if (!FindTypedResource(6, group, &bytes, &byteCount)) return 0; // RT_STRING

    Reader reader(bytes, bytes + byteCount);
    for (UINT index = 0; index < 16; ++index)
    {
        WORD length = 0;
        if (!reader.Word(&length) || length > byteCount / sizeof(wchar_t)) return 0;
        if (index != (identifier & 0x0f))
        {
            if (!reader.Skip(static_cast<size_t>(length) * sizeof(wchar_t))) return 0;
            continue;
        }
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1, L'\0');
        for (WORD character = 0; character < length; ++character)
        {
            WORD value = 0;
            if (!reader.Word(&value)) return 0;
            text[character] = static_cast<wchar_t>(value);
        }
        if (!buffer || bufferCount <= 0) return length;
        const int copied = (std::min)(static_cast<int>(length), bufferCount - 1);
        if (copied > 0) std::memcpy(buffer, text.data(), static_cast<size_t>(copied) * sizeof(wchar_t));
        buffer[copied] = L'\0';
        return copied;
    }
    return 0;
}
