#pragma once
#include <objidl.h>
#include <memory>
namespace Win32Bridge { namespace Bridge {
class GuestStorageContext;
HRESULT OpenGuestFileStream(LPCWSTR path, DWORD access, IStream** stream, const std::shared_ptr<GuestStorageContext>& storage = {});
HRESULT BorrowGuestFileStream(HANDLE file, IStream** stream, const std::shared_ptr<GuestStorageContext>& storage = {});
} }