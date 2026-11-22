#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <cstdlib>
#include <cwchar>
#include <ctype.h>
#include <wctype.h>
namespace Win32Bridge { namespace Bridge {
inline int __cdecl CrtLocaleData_isalnum(int value){return ::_isalnum_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isalpha(int value){return ::_isalpha_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iscntrl(int value){return ::_iscntrl_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isdigit(int value){return ::_isdigit_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isgraph(int value){return ::_isgraph_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_islower(int value){return ::_islower_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isprint(int value){return ::_isprint_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_ispunct(int value){return ::_ispunct_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isspace(int value){return ::_isspace_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isupper(int value){return ::_isupper_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isxdigit(int value){return ::_isxdigit_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_tolower(int value){return ::_tolower_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_toupper(int value){return ::_toupper_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswalnum(wint_t value){return ::_iswalnum_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswalpha(wint_t value){return ::_iswalpha_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswcntrl(wint_t value){return ::_iswcntrl_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswdigit(wint_t value){return ::_iswdigit_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswgraph(wint_t value){return ::_iswgraph_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswlower(wint_t value){return ::_iswlower_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswprint(wint_t value){return ::_iswprint_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswpunct(wint_t value){return ::_iswpunct_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswspace(wint_t value){return ::_iswspace_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswupper(wint_t value){return ::_iswupper_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswxdigit(wint_t value){return ::_iswxdigit_l(value,CurrentCrtLocaleHandle());}
inline wint_t __cdecl CrtLocaleData_towlower(wint_t value){return ::_towlower_l(value,CurrentCrtLocaleHandle());}
inline wint_t __cdecl CrtLocaleData_towupper(wint_t value){return ::_towupper_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_atoi(const char* text){return ::_atoi_l(text,CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData_atol(const char* text){return ::_atol_l(text,CurrentCrtLocaleHandle());}
inline long long __cdecl CrtLocaleData_atoll(const char* text){return ::_atoll_l(text,CurrentCrtLocaleHandle());}
inline __int64 __cdecl CrtLocaleData__atoi64(const char* text){return ::_atoi64_l(text,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__wtoi(const wchar_t* text){return ::_wtoi_l(text,CurrentCrtLocaleHandle());}
inline __int64 __cdecl CrtLocaleData__wtoi64(const wchar_t* text){return ::_wtoi64_l(text,CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData__wtol(const wchar_t* text){return ::_wtol_l(text,CurrentCrtLocaleHandle());}
inline double __cdecl CrtLocaleData_strtod(const char* text,char** end){return ::_strtod_l(text,end,CurrentCrtLocaleHandle());}
inline float __cdecl CrtLocaleData_strtof(const char* text,char** end){return ::_strtof_l(text,end,CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData_strtol(const char* text,char** end,int base){return ::_strtol_l(text,end,base,CurrentCrtLocaleHandle());}
inline long long __cdecl CrtLocaleData_strtoll(const char* text,char** end,int base){return ::_strtoll_l(text,end,base,CurrentCrtLocaleHandle());}
inline unsigned long __cdecl CrtLocaleData_strtoul(const char* text,char** end,int base){return ::_strtoul_l(text,end,base,CurrentCrtLocaleHandle());}
inline unsigned long long __cdecl CrtLocaleData_strtoull(const char* text,char** end,int base){return ::_strtoull_l(text,end,base,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__strlwr_s(char* text,size_t size){return ::_strlwr_s_l(text,size,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__strupr_s(char* text,size_t size){return ::_strupr_s_l(text,size,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__stricmp(const char* left,const char* right){return ::_stricmp_l(left,right,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__strnicmp(const char* left,const char* right,size_t count){return ::_strnicmp_l(left,right,count,CurrentCrtLocaleHandle());}
inline double __cdecl CrtLocaleData_wcstod(const wchar_t* text,wchar_t** end){return ::_wcstod_l(text,end,CurrentCrtLocaleHandle());}
inline float __cdecl CrtLocaleData_wcstof(const wchar_t* text,wchar_t** end){return ::_wcstof_l(text,end,CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData_wcstol(const wchar_t* text,wchar_t** end,int base){return ::_wcstol_l(text,end,base,CurrentCrtLocaleHandle());}
inline long long __cdecl CrtLocaleData_wcstoll(const wchar_t* text,wchar_t** end,int base){return ::_wcstoll_l(text,end,base,CurrentCrtLocaleHandle());}
inline unsigned long __cdecl CrtLocaleData_wcstoul(const wchar_t* text,wchar_t** end,int base){return ::_wcstoul_l(text,end,base,CurrentCrtLocaleHandle());}
inline unsigned long long __cdecl CrtLocaleData_wcstoull(const wchar_t* text,wchar_t** end,int base){return ::_wcstoull_l(text,end,base,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__wcslwr_s(wchar_t* text,size_t size){return ::_wcslwr_s_l(text,size,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__wcsupr_s(wchar_t* text,size_t size){return ::_wcsupr_s_l(text,size,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__wcsicmp(const wchar_t* left,const wchar_t* right){return ::_wcsicmp_l(left,right,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__wcsnicmp(const wchar_t* left,const wchar_t* right,size_t count){return ::_wcsnicmp_l(left,right,count,CurrentCrtLocaleHandle());}
// Explicit _l APIs also interpret a null handle as the guest default.
inline int __cdecl CrtLocaleData_isalnum_l(int value,_locale_t locale){return ::_isalnum_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isalpha_l(int value,_locale_t locale){return ::_isalpha_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iscntrl_l(int value,_locale_t locale){return ::_iscntrl_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isdigit_l(int value,_locale_t locale){return ::_isdigit_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isgraph_l(int value,_locale_t locale){return ::_isgraph_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_islower_l(int value,_locale_t locale){return ::_islower_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isprint_l(int value,_locale_t locale){return ::_isprint_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_ispunct_l(int value,_locale_t locale){return ::_ispunct_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isspace_l(int value,_locale_t locale){return ::_isspace_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isupper_l(int value,_locale_t locale){return ::_isupper_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_isxdigit_l(int value,_locale_t locale){return ::_isxdigit_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_tolower_l(int value,_locale_t locale){return ::_tolower_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_toupper_l(int value,_locale_t locale){return ::_toupper_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswalnum_l(wint_t value,_locale_t locale){return ::_iswalnum_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswalpha_l(wint_t value,_locale_t locale){return ::_iswalpha_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswcntrl_l(wint_t value,_locale_t locale){return ::_iswcntrl_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswdigit_l(wint_t value,_locale_t locale){return ::_iswdigit_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswgraph_l(wint_t value,_locale_t locale){return ::_iswgraph_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswlower_l(wint_t value,_locale_t locale){return ::_iswlower_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswprint_l(wint_t value,_locale_t locale){return ::_iswprint_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswpunct_l(wint_t value,_locale_t locale){return ::_iswpunct_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswspace_l(wint_t value,_locale_t locale){return ::_iswspace_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswupper_l(wint_t value,_locale_t locale){return ::_iswupper_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_iswxdigit_l(wint_t value,_locale_t locale){return ::_iswxdigit_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline wint_t __cdecl CrtLocaleData_towlower_l(wint_t value,_locale_t locale){return ::_towlower_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline wint_t __cdecl CrtLocaleData_towupper_l(wint_t value,_locale_t locale){return ::_towupper_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData_atoi_l(const char* text,_locale_t locale){return ::_atoi_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData_atol_l(const char* text,_locale_t locale){return ::_atol_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline long long __cdecl CrtLocaleData_atoll_l(const char* text,_locale_t locale){return ::_atoll_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline __int64 __cdecl CrtLocaleData__atoi64_l(const char* text,_locale_t locale){return ::_atoi64_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__wtoi_l(const wchar_t* text,_locale_t locale){return ::_wtoi_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline __int64 __cdecl CrtLocaleData__wtoi64_l(const wchar_t* text,_locale_t locale){return ::_wtoi64_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData__wtol_l(const wchar_t* text,_locale_t locale){return ::_wtol_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline double __cdecl CrtLocaleData_strtod_l(const char* text,char** end,_locale_t locale){return ::_strtod_l(text,end,locale?locale:CurrentCrtLocaleHandle());}
inline float __cdecl CrtLocaleData_strtof_l(const char* text,char** end,_locale_t locale){return ::_strtof_l(text,end,locale?locale:CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData_strtol_l(const char* text,char** end,int base,_locale_t locale){return ::_strtol_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline long long __cdecl CrtLocaleData_strtoll_l(const char* text,char** end,int base,_locale_t locale){return ::_strtoll_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned long __cdecl CrtLocaleData_strtoul_l(const char* text,char** end,int base,_locale_t locale){return ::_strtoul_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned long long __cdecl CrtLocaleData_strtoull_l(const char* text,char** end,int base,_locale_t locale){return ::_strtoull_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__strlwr_s_l(char* text,size_t size,_locale_t locale){return ::_strlwr_s_l(text,size,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__strupr_s_l(char* text,size_t size,_locale_t locale){return ::_strupr_s_l(text,size,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__stricmp_l(const char* left,const char* right,_locale_t locale){return ::_stricmp_l(left,right,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__strnicmp_l(const char* left,const char* right,size_t count,_locale_t locale){return ::_strnicmp_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline double __cdecl CrtLocaleData_wcstod_l(const wchar_t* text,wchar_t** end,_locale_t locale){return ::_wcstod_l(text,end,locale?locale:CurrentCrtLocaleHandle());}
inline float __cdecl CrtLocaleData_wcstof_l(const wchar_t* text,wchar_t** end,_locale_t locale){return ::_wcstof_l(text,end,locale?locale:CurrentCrtLocaleHandle());}
inline long __cdecl CrtLocaleData_wcstol_l(const wchar_t* text,wchar_t** end,int base,_locale_t locale){return ::_wcstol_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline long long __cdecl CrtLocaleData_wcstoll_l(const wchar_t* text,wchar_t** end,int base,_locale_t locale){return ::_wcstoll_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned long __cdecl CrtLocaleData_wcstoul_l(const wchar_t* text,wchar_t** end,int base,_locale_t locale){return ::_wcstoul_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned long long __cdecl CrtLocaleData_wcstoull_l(const wchar_t* text,wchar_t** end,int base,_locale_t locale){return ::_wcstoull_l(text,end,base,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__wcslwr_s_l(wchar_t* text,size_t size,_locale_t locale){return ::_wcslwr_s_l(text,size,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleData__wcsupr_s_l(wchar_t* text,size_t size,_locale_t locale){return ::_wcsupr_s_l(text,size,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__wcsicmp_l(const wchar_t* left,const wchar_t* right,_locale_t locale){return ::_wcsicmp_l(left,right,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleData__wcsnicmp_l(const wchar_t* left,const wchar_t* right,size_t count,_locale_t locale){return ::_wcsnicmp_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
// These conversions follow LC_CTYPE, independently of _setmbcp.
#pragma warning(push)
#pragma warning(disable:4996)
inline int __cdecl CrtLocaleConvert_mblen(const char* text,size_t count){return ::_mblen_l(text,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleConvert__mblen_l(const char* text,size_t count,_locale_t locale){return ::_mblen_l(text,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleConvert_mbtowc(wchar_t* target,const char* text,size_t count){return ::_mbtowc_l(target,text,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleConvert__mbtowc_l(wchar_t* target,const char* text,size_t count,_locale_t locale){return ::_mbtowc_l(target,text,count,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert_mbstowcs(wchar_t* target,const char* text,size_t count){return ::_mbstowcs_l(target,text,count,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert__mbstowcs_l(wchar_t* target,const char* text,size_t count,_locale_t locale){return ::_mbstowcs_l(target,text,count,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleConvert_mbstowcs_s(size_t* converted,wchar_t* target,size_t capacity,const char* text,size_t count){return ::_mbstowcs_s_l(converted,target,capacity,text,count,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleConvert__mbstowcs_s_l(size_t* converted,wchar_t* target,size_t capacity,const char* text,size_t count,_locale_t locale){return ::_mbstowcs_s_l(converted,target,capacity,text,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleConvert_wctomb(char* target,wchar_t character){return ::_wctomb_l(target,character,CurrentCrtLocaleHandle());}
inline int __cdecl CrtLocaleConvert__wctomb_l(char* target,wchar_t character,_locale_t locale){return ::_wctomb_l(target,character,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleConvert_wctomb_s(int* converted,char* target,size_t capacity,wchar_t character){return ::_wctomb_s_l(converted,target,capacity,character,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleConvert__wctomb_s_l(int* converted,char* target,size_t capacity,wchar_t character,_locale_t locale){return ::_wctomb_s_l(converted,target,capacity,character,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert_wcstombs(char* target,const wchar_t* text,size_t count){return ::_wcstombs_l(target,text,count,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert__wcstombs_l(char* target,const wchar_t* text,size_t count,_locale_t locale){return ::_wcstombs_l(target,text,count,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleConvert_wcstombs_s(size_t* converted,char* target,size_t capacity,const wchar_t* text,size_t count){return ::_wcstombs_s_l(converted,target,capacity,text,count,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtLocaleConvert__wcstombs_s_l(size_t* converted,char* target,size_t capacity,const wchar_t* text,size_t count,_locale_t locale){return ::_wcstombs_s_l(converted,target,capacity,text,count,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert__mbstrlen(const char* text){return ::_mbstrlen_l(text,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert__mbstrlen_l(const char* text,_locale_t locale){return ::_mbstrlen_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert__mbstrnlen(const char* text,size_t count){return ::_mbstrnlen_l(text,count,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtLocaleConvert__mbstrnlen_l(const char* text,size_t count,_locale_t locale){return ::_mbstrnlen_l(text,count,locale?locale:CurrentCrtLocaleHandle());}
#pragma warning(pop)
} }
