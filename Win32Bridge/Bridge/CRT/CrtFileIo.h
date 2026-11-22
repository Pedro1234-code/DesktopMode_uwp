#pragma once
#include "Bridge/CRT/CrtFileDescriptors.h"
namespace Win32Bridge { namespace Bridge {
    class GuestStorageContext;
    class GuestKernelContext;
    std::shared_ptr<CrtFileBackend> CreateGuestCrtFileBackend(std::shared_ptr<GuestStorageContext> storage, std::shared_ptr<GuestKernelContext> kernel = {});
} }