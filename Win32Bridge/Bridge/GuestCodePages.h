#pragma once
namespace Win32Bridge { namespace Bridge {
// Shared guest NLS defaults. CRT pseudo-code pages must resolve to the same
// environment as GetACP/GetOEMCP, independently of the host machine's locale.
constexpr unsigned GuestAnsiCodePage=1252;
constexpr unsigned GuestOemCodePage=437;
} }
