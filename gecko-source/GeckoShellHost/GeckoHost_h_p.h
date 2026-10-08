

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 8.01.0628 */
/* at Tue Jan 19 00:14:07 2038
 */
/* Compiler settings for C:\Users\fcvin\AppData\Local\Temp\GeckoHost.idl-aab6e6f0:
    Oicf, W1, Zp8, env=Win64 (32b run), target_arch=AMD64 8.01.0628 
    protocol : all , ms_ext, c_ext, robust
    error checks: allocation ref bounds_check enum stub_data 
    VC __declspec() decoration level: 
         __declspec(uuid()), __declspec(selectany), __declspec(novtable)
         DECLSPEC_UUID(), MIDL_INTERFACE()
*/
/* @@MIDL_FILE_HEADING(  ) */



/* verify that the <rpcndr.h> version is high enough to compile this file*/
#ifndef __REQUIRED_RPCNDR_H_VERSION__
#define __REQUIRED_RPCNDR_H_VERSION__ 500
#endif

#include "rpc.h"
#include "rpcndr.h"

#ifndef __RPCNDR_H_VERSION__
#error this stub requires an updated version of <rpcndr.h>
#endif /* __RPCNDR_H_VERSION__ */

#ifndef COM_NO_WINDOWS_H
#include "windows.h"
#include "ole2.h"
#endif /*COM_NO_WINDOWS_H*/

#ifndef __GeckoHost_h_p_h__
#define __GeckoHost_h_p_h__

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

#ifndef DECLSPEC_XFGVIRT
#if defined(_CONTROL_FLOW_GUARD_XFG)
#define DECLSPEC_XFGVIRT(base, func) __declspec(xfg_virtual(base, func))
#else
#define DECLSPEC_XFGVIRT(base, func)
#endif
#endif

#if defined(__cplusplus)
#if defined(__MIDL_USE_C_ENUM)
#define MIDL_ENUM enum
#else
#define MIDL_ENUM enum class
#endif
#endif


/* Forward Declarations */ 

#ifndef ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_FWD_DEFINED__
#define ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_FWD_DEFINED__
typedef interface __x_ABI_CDesktopMode_CGecko_CIGeckoHost __x_ABI_CDesktopMode_CGecko_CIGeckoHost;

#endif 	/* ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_FWD_DEFINED__ */


/* header files for imported files */
#include "inspectable.h"

#ifdef __cplusplus
extern "C"{
#endif 


/* interface __MIDL_itf_GeckoHost_0000_0000 */
/* [local] */ 





extern RPC_IF_HANDLE __MIDL_itf_GeckoHost_0000_0000_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_GeckoHost_0000_0000_v0_0_s_ifspec;

#ifndef ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_INTERFACE_DEFINED__
#define ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_INTERFACE_DEFINED__

/* interface __x_ABI_CDesktopMode_CGecko_CIGeckoHost */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CDesktopMode_CGecko_CIGeckoHost;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("bf8d71f5-29cb-59ce-8b6c-0059901fc71a")
    __x_ABI_CDesktopMode_CGecko_CIGeckoHost : public IInspectable
    {
    public:
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Content( 
            /* [retval][out] */ IInspectable **value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsStarted( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Start( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetViewport( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetActive( 
            /* [in] */ boolean active) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE OpenUrl( 
            /* [in] */ HSTRING url) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Suspend( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Resume( void) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CDesktopMode_CGecko_CIGeckoHostVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, get_Content)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Content )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ IInspectable **value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, get_IsStarted)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsStarted )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ boolean *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, Start)
        HRESULT ( STDMETHODCALLTYPE *Start )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ double width,
            /* [in] */ double height);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, SetViewport)
        HRESULT ( STDMETHODCALLTYPE *SetViewport )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ double width,
            /* [in] */ double height);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, SetActive)
        HRESULT ( STDMETHODCALLTYPE *SetActive )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ boolean active);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, OpenUrl)
        HRESULT ( STDMETHODCALLTYPE *OpenUrl )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ HSTRING url);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, Suspend)
        HRESULT ( STDMETHODCALLTYPE *Suspend )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, Resume)
        HRESULT ( STDMETHODCALLTYPE *Resume )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        END_INTERFACE
    } __x_ABI_CDesktopMode_CGecko_CIGeckoHostVtbl;

    interface __x_ABI_CDesktopMode_CGecko_CIGeckoHost
    {
        CONST_VTBL struct __x_ABI_CDesktopMode_CGecko_CIGeckoHostVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_get_Content(This,value)	\
    ( (This)->lpVtbl -> get_Content(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_get_IsStarted(This,value)	\
    ( (This)->lpVtbl -> get_IsStarted(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Start(This,width,height)	\
    ( (This)->lpVtbl -> Start(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetViewport(This,width,height)	\
    ( (This)->lpVtbl -> SetViewport(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetActive(This,active)	\
    ( (This)->lpVtbl -> SetActive(This,active) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_OpenUrl(This,url)	\
    ( (This)->lpVtbl -> OpenUrl(This,url) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Suspend(This)	\
    ( (This)->lpVtbl -> Suspend(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Resume(This)	\
    ( (This)->lpVtbl -> Resume(This) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_INTERFACE_DEFINED__ */


/* Additional Prototypes for ALL interfaces */

unsigned long             __RPC_USER  HSTRING_UserSize(     unsigned long *, unsigned long            , HSTRING * ); 
unsigned char * __RPC_USER  HSTRING_UserMarshal(  unsigned long *, unsigned char *, HSTRING * ); 
unsigned char * __RPC_USER  HSTRING_UserUnmarshal(unsigned long *, unsigned char *, HSTRING * ); 
void                      __RPC_USER  HSTRING_UserFree(     unsigned long *, HSTRING * ); 

unsigned long             __RPC_USER  HSTRING_UserSize64(     unsigned long *, unsigned long            , HSTRING * ); 
unsigned char * __RPC_USER  HSTRING_UserMarshal64(  unsigned long *, unsigned char *, HSTRING * ); 
unsigned char * __RPC_USER  HSTRING_UserUnmarshal64(unsigned long *, unsigned char *, HSTRING * ); 
void                      __RPC_USER  HSTRING_UserFree64(     unsigned long *, HSTRING * ); 

/* end of Additional Prototypes */

#ifdef __cplusplus
}
#endif

#endif


