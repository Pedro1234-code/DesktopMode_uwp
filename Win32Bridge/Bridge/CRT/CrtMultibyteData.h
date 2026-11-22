#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <mbctype.h>
#include <mbstring.h>
// The SDK hides these memory-only UCRT declarations in UWP builds even
// though their exports are in the CRT import library. Declare only the exact
// native signatures used here; do not change the SDK's API-family macros.
#ifndef _CRT_USE_WINAPI_FAMILY_DESKTOP_APP
extern "C" {
    _DCRTIMP int __cdecl _ismbcalnum_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcalpha_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcblank_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcdigit_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcgraph_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbchira_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbckata_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcl0_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcl1_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcl2_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbclower_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbclegal_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcprint_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcpunct_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcspace_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcsymbol_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _ismbcupper_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbbtombc_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbctombb_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbctolower_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbctoupper_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbcjistojms_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbcjmstojis_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbctohira_l(unsigned int value,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbctokata_l(unsigned int value,_locale_t locale);
    _DCRTIMP int __cdecl _mbbtype_l(unsigned char value,int previousType,_locale_t locale);
    _DCRTIMP int __cdecl _mbsbtype_l(const unsigned char* text,size_t position,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbslen_l(const unsigned char* text,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbclen_l(const unsigned char* text,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbsnlen_l(const unsigned char* text,size_t count,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbsnbcnt_l(const unsigned char* text,size_t count,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbsnccnt_l(const unsigned char* text,size_t count,_locale_t locale);
    _DCRTIMP unsigned char* __cdecl _mbsinc_l(const unsigned char* text,_locale_t locale);
    _DCRTIMP unsigned char* __cdecl _mbsninc_l(const unsigned char* text,size_t count,_locale_t locale);
    _DCRTIMP unsigned char* __cdecl _mbsdec_l(const unsigned char* start,const unsigned char* position,_locale_t locale);
    _DCRTIMP unsigned int __cdecl _mbsnextc_l(const unsigned char* text,_locale_t locale);
    _DCRTIMP int __cdecl _mbscmp_l(const unsigned char* left,const unsigned char* right,_locale_t locale);
    _DCRTIMP int __cdecl _mbsicmp_l(const unsigned char* left,const unsigned char* right,_locale_t locale);
    _DCRTIMP int __cdecl _mbscoll_l(const unsigned char* left,const unsigned char* right,_locale_t locale);
    _DCRTIMP int __cdecl _mbsicoll_l(const unsigned char* left,const unsigned char* right,_locale_t locale);
    _DCRTIMP int __cdecl _mbsncmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsnicmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsnbcmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsnbicmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsncoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsnicoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsnbcoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP int __cdecl _mbsnbicoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale);
    _DCRTIMP const unsigned char* __cdecl _mbschr_l(const unsigned char* text,unsigned int value,_locale_t locale);
    _DCRTIMP const unsigned char* __cdecl _mbsrchr_l(const unsigned char* text,unsigned int value,_locale_t locale);
    _DCRTIMP const unsigned char* __cdecl _mbsstr_l(const unsigned char* text,const unsigned char* find,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbsspn_l(const unsigned char* text,const unsigned char* set,_locale_t locale);
    _DCRTIMP size_t __cdecl _mbscspn_l(const unsigned char* text,const unsigned char* set,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbslwr_s_l(unsigned char* text,size_t size,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbsupr_s_l(unsigned char* text,size_t size,_locale_t locale);
    _DCRTIMP unsigned char* __cdecl _mbslwr_l(unsigned char* text,_locale_t locale);
    _DCRTIMP unsigned char* __cdecl _mbsupr_l(unsigned char* text,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbscpy_s_l(unsigned char* target,size_t capacity,const unsigned char* source,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbscat_s_l(unsigned char* target,size_t capacity,const unsigned char* source,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbsncpy_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbsnbcpy_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbsncat_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale);
    _DCRTIMP errno_t __cdecl _mbsnbcat_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale);
    _DCRTIMP unsigned char* __cdecl _mbstok_s_l(unsigned char* text,const unsigned char* delimiters,unsigned char** context,_locale_t locale);
}
#endif
namespace Win32Bridge { namespace Bridge {
// Memory-only native _l operations retain guest MBCS state via the boundary.
#pragma warning(push)
#pragma warning(disable:4996)
inline int __cdecl CrtMb_ismbbalnum(unsigned int value){return ::_ismbbalnum_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbalnum_l(unsigned int value,_locale_t locale){return ::_ismbbalnum_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbalpha(unsigned int value){return ::_ismbbalpha_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbalpha_l(unsigned int value,_locale_t locale){return ::_ismbbalpha_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbblank(unsigned int value){return ::_ismbbblank_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbblank_l(unsigned int value,_locale_t locale){return ::_ismbbblank_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbgraph(unsigned int value){return ::_ismbbgraph_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbgraph_l(unsigned int value,_locale_t locale){return ::_ismbbgraph_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkalnum(unsigned int value){return ::_ismbbkalnum_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkalnum_l(unsigned int value,_locale_t locale){return ::_ismbbkalnum_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkana(unsigned int value){return ::_ismbbkana_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkana_l(unsigned int value,_locale_t locale){return ::_ismbbkana_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkprint(unsigned int value){return ::_ismbbkprint_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkprint_l(unsigned int value,_locale_t locale){return ::_ismbbkprint_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkpunct(unsigned int value){return ::_ismbbkpunct_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbkpunct_l(unsigned int value,_locale_t locale){return ::_ismbbkpunct_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbblead(unsigned int value){return ::_ismbblead_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbblead_l(unsigned int value,_locale_t locale){return ::_ismbblead_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbprint(unsigned int value){return ::_ismbbprint_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbprint_l(unsigned int value,_locale_t locale){return ::_ismbbprint_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbpunct(unsigned int value){return ::_ismbbpunct_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbpunct_l(unsigned int value,_locale_t locale){return ::_ismbbpunct_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbtrail(unsigned int value){return ::_ismbbtrail_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbbtrail_l(unsigned int value,_locale_t locale){return ::_ismbbtrail_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcalnum(unsigned int value){return ::_ismbcalnum_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcalnum_l(unsigned int value,_locale_t locale){return ::_ismbcalnum_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcalpha(unsigned int value){return ::_ismbcalpha_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcalpha_l(unsigned int value,_locale_t locale){return ::_ismbcalpha_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcblank(unsigned int value){return ::_ismbcblank_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcblank_l(unsigned int value,_locale_t locale){return ::_ismbcblank_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcdigit(unsigned int value){return ::_ismbcdigit_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcdigit_l(unsigned int value,_locale_t locale){return ::_ismbcdigit_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcgraph(unsigned int value){return ::_ismbcgraph_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcgraph_l(unsigned int value,_locale_t locale){return ::_ismbcgraph_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbchira(unsigned int value){return ::_ismbchira_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbchira_l(unsigned int value,_locale_t locale){return ::_ismbchira_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbckata(unsigned int value){return ::_ismbckata_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbckata_l(unsigned int value,_locale_t locale){return ::_ismbckata_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcl0(unsigned int value){return ::_ismbcl0_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcl0_l(unsigned int value,_locale_t locale){return ::_ismbcl0_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcl1(unsigned int value){return ::_ismbcl1_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcl1_l(unsigned int value,_locale_t locale){return ::_ismbcl1_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcl2(unsigned int value){return ::_ismbcl2_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcl2_l(unsigned int value,_locale_t locale){return ::_ismbcl2_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbclower(unsigned int value){return ::_ismbclower_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbclower_l(unsigned int value,_locale_t locale){return ::_ismbclower_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbclegal(unsigned int value){return ::_ismbclegal_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbclegal_l(unsigned int value,_locale_t locale){return ::_ismbclegal_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcprint(unsigned int value){return ::_ismbcprint_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcprint_l(unsigned int value,_locale_t locale){return ::_ismbcprint_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcpunct(unsigned int value){return ::_ismbcpunct_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcpunct_l(unsigned int value,_locale_t locale){return ::_ismbcpunct_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcspace(unsigned int value){return ::_ismbcspace_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcspace_l(unsigned int value,_locale_t locale){return ::_ismbcspace_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcsymbol(unsigned int value){return ::_ismbcsymbol_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcsymbol_l(unsigned int value,_locale_t locale){return ::_ismbcsymbol_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcupper(unsigned int value){return ::_ismbcupper_l(value,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbcupper_l(unsigned int value,_locale_t locale){return ::_ismbcupper_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbbtombc(unsigned int value){return ::_mbbtombc_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbbtombc_l(unsigned int value,_locale_t locale){return ::_mbbtombc_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctombb(unsigned int value){return ::_mbctombb_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctombb_l(unsigned int value,_locale_t locale){return ::_mbctombb_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctolower(unsigned int value){return ::_mbctolower_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctolower_l(unsigned int value,_locale_t locale){return ::_mbctolower_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctoupper(unsigned int value){return ::_mbctoupper_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctoupper_l(unsigned int value,_locale_t locale){return ::_mbctoupper_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbcjistojms(unsigned int value){return ::_mbcjistojms_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbcjistojms_l(unsigned int value,_locale_t locale){return ::_mbcjistojms_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbcjmstojis(unsigned int value){return ::_mbcjmstojis_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbcjmstojis_l(unsigned int value,_locale_t locale){return ::_mbcjmstojis_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctohira(unsigned int value){return ::_mbctohira_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctohira_l(unsigned int value,_locale_t locale){return ::_mbctohira_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctokata(unsigned int value){return ::_mbctokata_l(value,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbctokata_l(unsigned int value,_locale_t locale){return ::_mbctokata_l(value,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbslead(const unsigned char* start,const unsigned char* position){return ::_ismbslead_l(start,position,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbslead_l(const unsigned char* start,const unsigned char* position,_locale_t locale){return ::_ismbslead_l(start,position,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbstrail(const unsigned char* start,const unsigned char* position){return ::_ismbstrail_l(start,position,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_ismbstrail_l(const unsigned char* start,const unsigned char* position,_locale_t locale){return ::_ismbstrail_l(start,position,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbbtype(unsigned char value,int previousType){return ::_mbbtype_l(value,previousType,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbbtype_l(unsigned char value,int previousType,_locale_t locale){return ::_mbbtype_l(value,previousType,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsbtype(const unsigned char* text,size_t position){return ::_mbsbtype_l(text,position,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsbtype_l(const unsigned char* text,size_t position,_locale_t locale){return ::_mbsbtype_l(text,position,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbslen(const unsigned char* text){return ::_mbslen_l(text,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbslen_l(const unsigned char* text,_locale_t locale){return ::_mbslen_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbclen(const unsigned char* text){return ::_mbclen_l(text,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbclen_l(const unsigned char* text,_locale_t locale){return ::_mbclen_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsnlen(const unsigned char* text,size_t count){return ::_mbsnlen_l(text,count,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsnlen_l(const unsigned char* text,size_t count,_locale_t locale){return ::_mbsnlen_l(text,count,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsnbcnt(const unsigned char* text,size_t count){return ::_mbsnbcnt_l(text,count,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsnbcnt_l(const unsigned char* text,size_t count,_locale_t locale){return ::_mbsnbcnt_l(text,count,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsnccnt(const unsigned char* text,size_t count){return ::_mbsnccnt_l(text,count,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsnccnt_l(const unsigned char* text,size_t count,_locale_t locale){return ::_mbsnccnt_l(text,count,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsinc(const unsigned char* text){return ::_mbsinc_l(text,CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsinc_l(const unsigned char* text,_locale_t locale){return ::_mbsinc_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsninc(const unsigned char* text,size_t count){return ::_mbsninc_l(text,count,CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsninc_l(const unsigned char* text,size_t count,_locale_t locale){return ::_mbsninc_l(text,count,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsdec(const unsigned char* start,const unsigned char* position){return ::_mbsdec_l(start,position,CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsdec_l(const unsigned char* start,const unsigned char* position,_locale_t locale){return ::_mbsdec_l(start,position,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbsnextc(const unsigned char* text){return ::_mbsnextc_l(text,CurrentCrtLocaleHandle());}
inline unsigned int __cdecl CrtMb_mbsnextc_l(const unsigned char* text,_locale_t locale){return ::_mbsnextc_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbscmp(const unsigned char* left,const unsigned char* right){return ::_mbscmp_l(left,right,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbscmp_l(const unsigned char* left,const unsigned char* right,_locale_t locale){return ::_mbscmp_l(left,right,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsicmp(const unsigned char* left,const unsigned char* right){return ::_mbsicmp_l(left,right,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsicmp_l(const unsigned char* left,const unsigned char* right,_locale_t locale){return ::_mbsicmp_l(left,right,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbscoll(const unsigned char* left,const unsigned char* right){return ::_mbscoll_l(left,right,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbscoll_l(const unsigned char* left,const unsigned char* right,_locale_t locale){return ::_mbscoll_l(left,right,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsicoll(const unsigned char* left,const unsigned char* right){return ::_mbsicoll_l(left,right,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsicoll_l(const unsigned char* left,const unsigned char* right,_locale_t locale){return ::_mbsicoll_l(left,right,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsncmp(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsncmp_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsncmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsncmp_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnicmp(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsnicmp_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnicmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsnicmp_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbcmp(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsnbcmp_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbcmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsnbcmp_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbicmp(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsnbicmp_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbicmp_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsnbicmp_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsncoll(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsncoll_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsncoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsncoll_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnicoll(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsnicoll_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnicoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsnicoll_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbcoll(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsnbcoll_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbcoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsnbcoll_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbicoll(const unsigned char* left,const unsigned char* right,size_t count){return ::_mbsnbicoll_l(left,right,count,CurrentCrtLocaleHandle());}
inline int __cdecl CrtMb_mbsnbicoll_l(const unsigned char* left,const unsigned char* right,size_t count,_locale_t locale){return ::_mbsnbicoll_l(left,right,count,locale?locale:CurrentCrtLocaleHandle());}
inline const unsigned char* __cdecl CrtMb_mbschr(const unsigned char* text,unsigned int value){return ::_mbschr_l(text,value,CurrentCrtLocaleHandle());}
inline const unsigned char* __cdecl CrtMb_mbschr_l(const unsigned char* text,unsigned int value,_locale_t locale){return ::_mbschr_l(text,value,locale?locale:CurrentCrtLocaleHandle());}
inline const unsigned char* __cdecl CrtMb_mbsrchr(const unsigned char* text,unsigned int value){return ::_mbsrchr_l(text,value,CurrentCrtLocaleHandle());}
inline const unsigned char* __cdecl CrtMb_mbsrchr_l(const unsigned char* text,unsigned int value,_locale_t locale){return ::_mbsrchr_l(text,value,locale?locale:CurrentCrtLocaleHandle());}
inline const unsigned char* __cdecl CrtMb_mbsstr(const unsigned char* text,const unsigned char* find){return ::_mbsstr_l(text,find,CurrentCrtLocaleHandle());}
inline const unsigned char* __cdecl CrtMb_mbsstr_l(const unsigned char* text,const unsigned char* find,_locale_t locale){return ::_mbsstr_l(text,find,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsspn(const unsigned char* text,const unsigned char* set){return ::_mbsspn_l(text,set,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbsspn_l(const unsigned char* text,const unsigned char* set,_locale_t locale){return ::_mbsspn_l(text,set,locale?locale:CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbscspn(const unsigned char* text,const unsigned char* set){return ::_mbscspn_l(text,set,CurrentCrtLocaleHandle());}
inline size_t __cdecl CrtMb_mbscspn_l(const unsigned char* text,const unsigned char* set,_locale_t locale){return ::_mbscspn_l(text,set,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbslwr_s(unsigned char* text,size_t size){return ::_mbslwr_s_l(text,size,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbslwr_s_l(unsigned char* text,size_t size,_locale_t locale){return ::_mbslwr_s_l(text,size,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsupr_s(unsigned char* text,size_t size){return ::_mbsupr_s_l(text,size,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsupr_s_l(unsigned char* text,size_t size,_locale_t locale){return ::_mbsupr_s_l(text,size,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbslwr(unsigned char* text){return ::_mbslwr_l(text,CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbslwr_l(unsigned char* text,_locale_t locale){return ::_mbslwr_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsupr(unsigned char* text){return ::_mbsupr_l(text,CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbsupr_l(unsigned char* text,_locale_t locale){return ::_mbsupr_l(text,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbscpy_s(unsigned char* target,size_t capacity,const unsigned char* source){return ::_mbscpy_s_l(target,capacity,source,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbscpy_s_l(unsigned char* target,size_t capacity,const unsigned char* source,_locale_t locale){return ::_mbscpy_s_l(target,capacity,source,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbscat_s(unsigned char* target,size_t capacity,const unsigned char* source){return ::_mbscat_s_l(target,capacity,source,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbscat_s_l(unsigned char* target,size_t capacity,const unsigned char* source,_locale_t locale){return ::_mbscat_s_l(target,capacity,source,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsncpy_s(unsigned char* target,size_t capacity,const unsigned char* source,size_t count){return ::_mbsncpy_s_l(target,capacity,source,count,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsncpy_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale){return ::_mbsncpy_s_l(target,capacity,source,count,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsnbcpy_s(unsigned char* target,size_t capacity,const unsigned char* source,size_t count){return ::_mbsnbcpy_s_l(target,capacity,source,count,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsnbcpy_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale){return ::_mbsnbcpy_s_l(target,capacity,source,count,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsncat_s(unsigned char* target,size_t capacity,const unsigned char* source,size_t count){return ::_mbsncat_s_l(target,capacity,source,count,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsncat_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale){return ::_mbsncat_s_l(target,capacity,source,count,locale?locale:CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsnbcat_s(unsigned char* target,size_t capacity,const unsigned char* source,size_t count){return ::_mbsnbcat_s_l(target,capacity,source,count,CurrentCrtLocaleHandle());}
inline errno_t __cdecl CrtMb_mbsnbcat_s_l(unsigned char* target,size_t capacity,const unsigned char* source,size_t count,_locale_t locale){return ::_mbsnbcat_s_l(target,capacity,source,count,locale?locale:CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbstok_s(unsigned char* text,const unsigned char* delimiters,unsigned char** context){return ::_mbstok_s_l(text,delimiters,context,CurrentCrtLocaleHandle());}
inline unsigned char* __cdecl CrtMb_mbstok_s_l(unsigned char* text,const unsigned char* delimiters,unsigned char** context,_locale_t locale){return ::_mbstok_s_l(text,delimiters,context,locale?locale:CurrentCrtLocaleHandle());}
#pragma warning(pop)
} }
