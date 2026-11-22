// Property page handles own a stable copy of the original A/W PSP structure.
// A sheet consumes a handle; it is never a cast of application stack memory.
namespace {
    constexpr DWORD PageIndirect = 0x00000001, PageIconName = 0x00000004, PageTitle = 0x00000008;
    constexpr DWORD PageRefParent = 0x00000040, PageCallback = 0x00000080;
    constexpr DWORD PageHeaderTitle = 0x00001000, PageHeaderSubtitle = 0x00002000, PageActivation = 0x00004000;
    using PropertyPageCallback = UINT(CALLBACK*)(HWND, UINT, void*);

    bool CopyPropertyBytes(void* target, const void* source, size_t size)
    {
        __try { memcpy(target, source, size); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ChangePropertyReference(UINT* reference, LONG increment)
    {
        if (!reference) return false;
        __try { ::InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(reference), increment); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool CallPropertyPageCallback(PropertyPageCallback callback, UINT message, void* page, UINT* result)
    {
        __try { return InvokeCrtFunction(result, callback, static_cast<HWND>(nullptr), message, page); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadPropertyAnsi(const char* source, std::string* result)
    {
        if (!result) return false;
        size_t count = 0;
        if (!BoundedPropertyTextLength(source, &count)) return false;
        result->assign(source, count); return true;
    }    bool PropertyAnsiToWide(const std::string& text, std::wstring* result)
    {
        if (text.empty()) { result->clear(); return true; }
        const int size = ::MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (!size) return false;
        result->resize(size);
        return ::MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), &(*result)[0], size) == size;
    }
    struct OwnedPropertyPage final
    {
        GuestModuleLoader* owner = nullptr;
        HANDLE handle = nullptr;
        bool unicode = true, referenceHeld = false, activationHeld = false, released = true;
        DWORD flags = 0;
        UINT* reference = nullptr;
        HANDLE activation = nullptr;
        PropertyPageCallback callback = nullptr;
        std::vector<BYTE> data;
        std::array<std::wstring, 5> wideStrings;
        std::array<std::string, 5> ansiStrings;
        std::wstring templateName, title;
        LPCWSTR normalizedTemplate = nullptr;
        void Release(bool notify = true)
        {
            if (released) return;
            released = true;
            if (notify && owner == CurrentGuestModuleLoader() && (flags & PageCallback) && callback)
            { UINT ignored = 0; CallPropertyPageCallback(callback, 1, data.data(), &ignored); } // PSPCB_RELEASE
            if (referenceHeld) { if (owner == CurrentGuestModuleLoader()) ChangePropertyReference(reference, -1); referenceHeld = false; }
            if (activationHeld) { if (owner == CurrentGuestModuleLoader()) BridgeReleaseActCtx(activation); activationHeld = false; }
        }
        ~OwnedPropertyPage() { Release(); }
    };
    std::mutex g_propertyPagesLock;
    std::unordered_map<ULONG_PTR, std::shared_ptr<OwnedPropertyPage>> g_propertyPages;
    std::unordered_map<ULONG_PTR, std::weak_ptr<OwnedPropertyPage>> g_propertyPageLifetimes;
    std::atomic<ULONG_PTR> g_nextPropertyPage{ 0x60000000 };

    bool CopyPropertyString(OwnedPropertyPage& page, size_t offset, size_t slot)
    {
        if (offset + sizeof(void*) > page.data.size()) return true;
        const void* original = nullptr; memcpy(&original, page.data.data() + offset, sizeof(original));
        if (reinterpret_cast<ULONG_PTR>(original) <= 0xffff) return true;
        const void* copied = nullptr;
        if (page.unicode)
        {
            if (!ReadBoundedGuestString(static_cast<LPCWSTR>(original), &page.wideStrings[slot])) return false;
            copied = page.wideStrings[slot].c_str();
        }
        else
        {
            if (!ReadPropertyAnsi(static_cast<const char*>(original), &page.ansiStrings[slot])) return false;
            copied = page.ansiStrings[slot].c_str();
        }
        memcpy(page.data.data() + offset, &copied, sizeof(copied));
        return true;
    }
    bool ReadPropertyText(const void* text, bool unicode, HINSTANCE instance, std::wstring* result)
    {
        if (!text) { result->clear(); return true; }
        if (reinterpret_cast<ULONG_PTR>(text) <= 0xffff)
        {
            wchar_t buffer[4096]{};
            const int count = LoadGuestStringResource(instance, static_cast<UINT>(reinterpret_cast<ULONG_PTR>(text)), buffer, 4096);
            if (count <= 0) return false;
            result->assign(buffer, count); return true;
        }
        if (unicode) return ReadBoundedGuestString(static_cast<LPCWSTR>(text), result);
        std::string narrow;
        return ReadPropertyAnsi(static_cast<const char*>(text), &narrow) && PropertyAnsiToWide(narrow, result);
    }
    HANDLE CreatePropertyPage(const void* source, bool unicode)
    {
        GuestPropertySheetPageW prefix{}; DWORD size = 0;
        if (!ReadPropertySheetPage(static_cast<const BYTE*>(source), &prefix, &size))
        { BridgeSetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        try
        {
            auto page = std::make_shared<OwnedPropertyPage>();
            page->owner = CurrentGuestModuleLoader(); page->unicode = unicode; page->flags = prefix.flags;
            page->callback = reinterpret_cast<PropertyPageCallback>(prefix.callback);
            page->reference = prefix.referenceCount;
            page->data.resize(size);
            // Release is meaningful only after the structure has been copied.
            page->released = true;
            if (!CopyPropertyBytes(page->data.data(), source, size))
            { BridgeSetLastError(ERROR_NOACCESS); return nullptr; }
            if (!(prefix.flags & PageIndirect) && !CopyPropertyString(*page, offsetof(GuestPropertySheetPageW, templateName), 0)) { BridgeSetLastError(ERROR_NOACCESS); return nullptr; }
            if ((prefix.flags & PageIconName) && !CopyPropertyString(*page, offsetof(GuestPropertySheetPageW, icon), 1)) { BridgeSetLastError(ERROR_NOACCESS); return nullptr; }
            if ((prefix.flags & PageTitle) && !CopyPropertyString(*page, offsetof(GuestPropertySheetPageW, title), 2)) { BridgeSetLastError(ERROR_NOACCESS); return nullptr; }
            if ((prefix.flags & PageHeaderTitle) && !CopyPropertyString(*page, 72, 3)) return nullptr;
            if ((prefix.flags & PageHeaderSubtitle) && !CopyPropertyString(*page, 80, 4)) return nullptr;
            GuestPropertySheetPageW copied{}; memcpy(&copied, page->data.data(), sizeof(copied));
            if (!(prefix.flags & PageIndirect))
            {
                if (reinterpret_cast<ULONG_PTR>(copied.templateName) <= 0xffff) page->normalizedTemplate = copied.templateName;
                else
                {
                    if (!ReadPropertyText(copied.templateName, unicode, prefix.instance, &page->templateName)) return nullptr;
                    page->normalizedTemplate = page->templateName.c_str();
                }
            }
            if ((prefix.flags & PageTitle) && copied.title)
                ReadPropertyText(copied.title, unicode, prefix.instance, &page->title);
            if ((prefix.flags & PageRefParent) && !ChangePropertyReference(page->reference, 1))
            { BridgeSetLastError(ERROR_NOACCESS); return nullptr; }
            page->referenceHeld = (prefix.flags & PageRefParent) != 0;
            page->released = false;
            if ((prefix.flags & PageActivation) && size >= 96)
            {
                memcpy(&page->activation, page->data.data() + 88, sizeof(page->activation));
                if (page->activation && page->activation != INVALID_HANDLE_VALUE)
                { BridgeAddRefActCtx(page->activation); page->activationHeld = true; }
            }
            page->released = false;
            if ((prefix.flags & PageCallback) && page->callback && size > sizeof(GuestPropertySheetPageW))
            {
                UINT ignored = 0;
                if (!CallPropertyPageCallback(page->callback, 0, page->data.data(), &ignored)) return nullptr; // PSPCB_ADDREF
            }
            page->handle = reinterpret_cast<HANDLE>(g_nextPropertyPage.fetch_add(1));
            {
                std::lock_guard<std::mutex> guard(g_propertyPagesLock);
                for (auto item = g_propertyPageLifetimes.begin(); item != g_propertyPageLifetimes.end(); )
                    if (item->second.expired()) item = g_propertyPageLifetimes.erase(item); else ++item;
                g_propertyPageLifetimes.emplace(reinterpret_cast<ULONG_PTR>(page->handle), page);
                g_propertyPages.emplace(reinterpret_cast<ULONG_PTR>(page->handle), page);
            }
            BridgeSetLastError(ERROR_SUCCESS);
            return page->handle;
        }
        catch (const std::bad_alloc&) { BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    }
    void ReleasePropertyPage(void* context) { if (context) static_cast<OwnedPropertyPage*>(context)->Release(); }
    bool PreparePropertyPage(void* context)
    {
        auto page = static_cast<OwnedPropertyPage*>(context);
        if (!page || page->released) return false;
        if (!(page->flags & PageCallback) || !page->callback) return true;
        UINT result = 0;
        return CallPropertyPageCallback(page->callback, 2, page->data.data(), &result) && result != 0; // PSPCB_CREATE
    }
    INT_PTR PropertySheetFromHeader(const void* source, bool unicode)
    {
        GuestPropertySheetHeaderW header{};
        if (!ReadPropertySheetHeader(source, &header) || !header.pages || !header.pageCount || header.pageCount > 256)
        { BridgeSetLastError(ERROR_INVALID_PARAMETER); return -1; }
        try
        {
            std::vector<GuestPropertyPageDescriptor> pages; pages.reserve(header.pageCount);
            const BYTE* cursor = reinterpret_cast<const BYTE*>(header.pages);
            for (UINT index = 0; index < header.pageCount; ++index)
            {
                HANDLE handle = nullptr;
                if (header.flags & PropertySheetHeaderPagesAreStructures)
                {
                    GuestPropertySheetPageW prefix{}; DWORD size = 0;
                    if (!ReadPropertySheetPage(cursor, &prefix, &size)) return -1;
                    handle = CreatePropertyPage(cursor, unicode); cursor += size;
                }
                else
                {
                    if (!CopyPropertyBytes(&handle, cursor, sizeof(handle))) return -1;
                    cursor += sizeof(handle);
                }
                if (!handle) return -1;
                GuestPropertyPageDescriptor page;
                if (!TakeGuestPropertySheetPage(handle, &page)) { BridgeDestroyPropertySheetPage(handle); return -1; }
                if (!page.instance) page.instance = header.instance;
                pages.push_back(std::move(page));
            }
            UINT selected = (header.flags & PropertySheetHeaderUsesStartPageName) ? 0 :
                (std::min)(static_cast<UINT>(header.startPage), header.pageCount - 1);
            if (header.flags & PropertySheetHeaderUsesStartPageName)
            {
                std::wstring requested;
                ReadPropertyText(reinterpret_cast<const void*>(header.startPage), unicode, header.instance, &requested);
                for (UINT index = 0; index < pages.size(); ++index)
                    if ((!requested.empty() && pages[index].title && !_wcsicmp(requested.c_str(), pages[index].title)) ||
                        (!requested.empty() && reinterpret_cast<ULONG_PTR>(pages[index].templateName) > 0xffff && !_wcsicmp(requested.c_str(), pages[index].templateName)))
                    { selected = index; break; }
            }
            std::wstring caption;
            ReadPropertyText(header.caption, unicode, header.instance, &caption);
            GuestPropertySheetDescriptor descriptor;
            descriptor.parent = header.parent; descriptor.instance = header.instance;
            descriptor.caption = caption.empty() ? nullptr : caption.c_str();
            descriptor.pages = pages.data(); descriptor.pageCount = static_cast<UINT>(pages.size());
            descriptor.startPage = selected; descriptor.flags = header.flags;
            if (header.flags & 0x100) descriptor.callback = reinterpret_cast<int(CALLBACK*)(HWND, UINT, LPARAM)>(header.callback);
            return ShowGuestPropertySheet(descriptor);
        }
        catch (const std::bad_alloc&) { BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return -1; }
    }
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreatePropertySheetPageW(const void* page) { return CreatePropertyPage(page, true); }
HANDLE WINAPI Win32Bridge::Bridge::BridgeCreatePropertySheetPageA(const void* page) { return CreatePropertyPage(page, false); }
INT_PTR WINAPI Win32Bridge::Bridge::BridgePropertySheetW(const void* header) { return PropertySheetFromHeader(header, true); }
INT_PTR WINAPI Win32Bridge::Bridge::BridgePropertySheetA(const void* header) { return PropertySheetFromHeader(header, false); }
BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyPropertySheetPage(HANDLE handle)
{
    std::shared_ptr<OwnedPropertyPage> page;
    {
        std::lock_guard<std::mutex> guard(g_propertyPagesLock);
        const auto found = g_propertyPages.find(reinterpret_cast<ULONG_PTR>(handle));
        if (found == g_propertyPages.end() || found->second->owner != CurrentGuestModuleLoader())
        { BridgeSetLastError(ERROR_INVALID_HANDLE); return FALSE; }
        page = found->second; g_propertyPages.erase(found);
    }
    page->Release(); BridgeSetLastError(ERROR_SUCCESS); return TRUE;
}
bool Win32Bridge::Bridge::TakeGuestPropertySheetPage(HANDLE handle, GuestPropertyPageDescriptor* descriptor, bool consume)
{
    if (!descriptor) return false;
    std::shared_ptr<OwnedPropertyPage> page;
    {
        std::lock_guard<std::mutex> guard(g_propertyPagesLock);
        const auto found = g_propertyPages.find(reinterpret_cast<ULONG_PTR>(handle));
        if (found == g_propertyPages.end() || found->second->owner != CurrentGuestModuleLoader())
        { BridgeSetLastError(ERROR_INVALID_HANDLE); return false; }
        page = found->second; if (consume) g_propertyPages.erase(found);
    }
    GuestPropertySheetPageW prefix{}; memcpy(&prefix, page->data.data(), sizeof(prefix));
    descriptor->instance = prefix.instance;
    descriptor->templateName = page->normalizedTemplate;
    descriptor->templateData = (prefix.flags & PageIndirect) ? prefix.templateName : nullptr;
    descriptor->dialogProcedure = prefix.dialogProcedure;
    descriptor->initParameter = reinterpret_cast<LPARAM>(page->data.data());
    descriptor->title = page->title.empty() ? nullptr : page->title.c_str();
    descriptor->pageFlags = prefix.flags; descriptor->pageHandle = handle;
    descriptor->activationContext = page->activation;
    descriptor->prepareCreate = &PreparePropertyPage; descriptor->release = &ReleasePropertyPage; descriptor->lifetime = page;
    return true;
}
void Win32Bridge::Bridge::ReleaseGuestPropertySheetPages(GuestModuleLoader* owner, bool notify)
{
    std::vector<std::shared_ptr<OwnedPropertyPage>> pages;
    {
        std::lock_guard<std::mutex> guard(g_propertyPagesLock);
        for (auto item = g_propertyPageLifetimes.begin(); item != g_propertyPageLifetimes.end(); )
        {
            auto page = item->second.lock();
            if (!page) { item = g_propertyPageLifetimes.erase(item); continue; }
            if (page->owner == owner)
            {
                pages.push_back(page); g_propertyPages.erase(item->first);
                item = g_propertyPageLifetimes.erase(item);
            }
            else ++item;
        }
    }
    for (auto item = pages.rbegin(); item != pages.rend(); ++item) (*item)->Release(notify);
}