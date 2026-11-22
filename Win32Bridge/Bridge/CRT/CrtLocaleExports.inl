namespace {
Win32Bridge::Bridge::ImportResolution ResolveCrtLocaleImport(const Win32Bridge::Bridge::ImportedSymbol& symbol){
    using namespace Win32Bridge::Bridge;
    ImportResolution result{ImportDisposition::Unsupported,L"No guest locale adapter.",0};
    if(symbol.importedByOrdinal||(_wcsicmp(symbol.library.c_str(),L"msvcrt.dll")&&_wcsicmp(symbol.library.c_str(),L"ucrtbase.dll")))return result;
#define CRT_LOCALE(exportName,function) if(symbol.name==L##exportName)result.targetAddress=reinterpret_cast<ULONGLONG>(&function);else
    CRT_LOCALE("setlocale",SetCrtLocale)
    CRT_LOCALE("_wsetlocale",SetCrtWideLocale)
    CRT_LOCALE("_configthreadlocale",ConfigureCrtLocale)
    CRT_LOCALE("localeconv",GetCrtLocaleConvention)
    CRT_LOCALE("_setmbcp",SetCrtMultibyteCodePage)
    CRT_LOCALE("_getmbcp",GetCrtMultibyteCodePage)
    CRT_LOCALE("_create_locale",::_create_locale)
    CRT_LOCALE("_wcreate_locale",::_wcreate_locale)
    CRT_LOCALE("_free_locale",::_free_locale)
    CRT_LOCALE("_get_current_locale",GetCrtCurrentLocale)
    {}
#undef CRT_LOCALE
    if(result.targetAddress){result.disposition=ImportDisposition::NeedsBridge;result.note=L"Guest process/thread locale with native immutable locale data.";}
    return result;
}
}
