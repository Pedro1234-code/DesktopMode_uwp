

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 8.01.0628 */
/* at Tue Jan 19 00:14:07 2038
 */
/* Compiler settings for C:\Users\fcvin\AppData\Local\Temp\DesktopMode.Gecko.idl-ffdc72d3:
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

#ifndef __DesktopMode2EGecko_h_p_h__
#define __DesktopMode2EGecko_h_p_h__

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

#ifndef ____x_ABI_CDesktopMode_CGecko_CIGeckoWindow_FWD_DEFINED__
#define ____x_ABI_CDesktopMode_CGecko_CIGeckoWindow_FWD_DEFINED__
typedef interface __x_ABI_CDesktopMode_CGecko_CIGeckoWindow __x_ABI_CDesktopMode_CGecko_CIGeckoWindow;

#endif 	/* ____x_ABI_CDesktopMode_CGecko_CIGeckoWindow_FWD_DEFINED__ */


#ifndef ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_FWD_DEFINED__
#define ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_FWD_DEFINED__
typedef interface __x_ABI_CDesktopMode_CGecko_CIGeckoHost __x_ABI_CDesktopMode_CGecko_CIGeckoHost;

#endif 	/* ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_FWD_DEFINED__ */


/* header files for imported files */
#include "inspectable.h"

#ifdef __cplusplus
extern "C"{
#endif 


/* interface __MIDL_itf_DesktopMode2EGecko_0000_0000 */
/* [local] */ 






extern RPC_IF_HANDLE __MIDL_itf_DesktopMode2EGecko_0000_0000_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_DesktopMode2EGecko_0000_0000_v0_0_s_ifspec;

#ifndef ____x_ABI_CDesktopMode_CGecko_CIGeckoWindow_INTERFACE_DEFINED__
#define ____x_ABI_CDesktopMode_CGecko_CIGeckoWindow_INTERFACE_DEFINED__

/* interface __x_ABI_CDesktopMode_CGecko_CIGeckoWindow */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CDesktopMode_CGecko_CIGeckoWindow;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("7bab581c-1234-5f70-bd4d-d77542d14486")
    __x_ABI_CDesktopMode_CGecko_CIGeckoWindow : public IInspectable
    {
    public:
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Content( 
            /* [retval][out] */ IInspectable **value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Id( 
            /* [retval][out] */ unsigned __int64 *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_State( 
            /* [retval][out] */ unsigned int *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Error( 
            /* [retval][out] */ unsigned int *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_TextInputState( 
            /* [retval][out] */ unsigned int *value) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE UpdateFrame( 
            /* [retval][out] */ boolean *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetViewport( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetActive( 
            /* [in] */ boolean active) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE OpenUrl( 
            /* [in] */ HSTRING url,
            /* [retval][out] */ boolean *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SendMouse( 
            /* [in] */ int kind,
            /* [in] */ int button,
            /* [in] */ double x,
            /* [in] */ double y) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SendWheel( 
            /* [in] */ double x,
            /* [in] */ double y,
            /* [in] */ double dx,
            /* [in] */ double dy) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SendKey( 
            /* [in] */ boolean down,
            /* [in] */ int key,
            /* [in] */ unsigned int modifiers,
            /* [in] */ boolean repeat) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SendText( 
            /* [in] */ HSTRING text) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE TakeWindowCommand( 
            /* [retval][out] */ int *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE ContextAction( 
            /* [in] */ HSTRING action,
            /* [retval][out] */ boolean *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE RequestClose( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Close( void) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CDesktopMode_CGecko_CIGeckoWindowVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, get_Content)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Content )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ IInspectable **value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, get_Id)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Id )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ unsigned __int64 *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, get_State)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_State )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ unsigned int *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, get_Error)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Error )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ unsigned int *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, get_TextInputState)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_TextInputState )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ unsigned int *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, UpdateFrame)
        HRESULT ( STDMETHODCALLTYPE *UpdateFrame )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ boolean *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, SetViewport)
        HRESULT ( STDMETHODCALLTYPE *SetViewport )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ double width,
            /* [in] */ double height);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, SetActive)
        HRESULT ( STDMETHODCALLTYPE *SetActive )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ boolean active);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, OpenUrl)
        HRESULT ( STDMETHODCALLTYPE *OpenUrl )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ HSTRING url,
            /* [retval][out] */ boolean *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, SendMouse)
        HRESULT ( STDMETHODCALLTYPE *SendMouse )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ int kind,
            /* [in] */ int button,
            /* [in] */ double x,
            /* [in] */ double y);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, SendWheel)
        HRESULT ( STDMETHODCALLTYPE *SendWheel )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ double x,
            /* [in] */ double y,
            /* [in] */ double dx,
            /* [in] */ double dy);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, SendKey)
        HRESULT ( STDMETHODCALLTYPE *SendKey )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ boolean down,
            /* [in] */ int key,
            /* [in] */ unsigned int modifiers,
            /* [in] */ boolean repeat);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, SendText)
        HRESULT ( STDMETHODCALLTYPE *SendText )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ HSTRING text);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, TakeWindowCommand)
        HRESULT ( STDMETHODCALLTYPE *TakeWindowCommand )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [retval][out] */ int *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, ContextAction)
        HRESULT ( STDMETHODCALLTYPE *ContextAction )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This,
            /* [in] */ HSTRING action,
            /* [retval][out] */ boolean *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, RequestClose)
        HRESULT ( STDMETHODCALLTYPE *RequestClose )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoWindow, Close)
        HRESULT ( STDMETHODCALLTYPE *Close )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoWindow * This);
        
        END_INTERFACE
    } __x_ABI_CDesktopMode_CGecko_CIGeckoWindowVtbl;

    interface __x_ABI_CDesktopMode_CGecko_CIGeckoWindow
    {
        CONST_VTBL struct __x_ABI_CDesktopMode_CGecko_CIGeckoWindowVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_get_Content(This,value)	\
    ( (This)->lpVtbl -> get_Content(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_get_Id(This,value)	\
    ( (This)->lpVtbl -> get_Id(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_get_State(This,value)	\
    ( (This)->lpVtbl -> get_State(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_get_Error(This,value)	\
    ( (This)->lpVtbl -> get_Error(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_get_TextInputState(This,value)	\
    ( (This)->lpVtbl -> get_TextInputState(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_UpdateFrame(This,result)	\
    ( (This)->lpVtbl -> UpdateFrame(This,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_SetViewport(This,width,height)	\
    ( (This)->lpVtbl -> SetViewport(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_SetActive(This,active)	\
    ( (This)->lpVtbl -> SetActive(This,active) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_OpenUrl(This,url,result)	\
    ( (This)->lpVtbl -> OpenUrl(This,url,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_SendMouse(This,kind,button,x,y)	\
    ( (This)->lpVtbl -> SendMouse(This,kind,button,x,y) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_SendWheel(This,x,y,dx,dy)	\
    ( (This)->lpVtbl -> SendWheel(This,x,y,dx,dy) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_SendKey(This,down,key,modifiers,repeat)	\
    ( (This)->lpVtbl -> SendKey(This,down,key,modifiers,repeat) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_SendText(This,text)	\
    ( (This)->lpVtbl -> SendText(This,text) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_TakeWindowCommand(This,result)	\
    ( (This)->lpVtbl -> TakeWindowCommand(This,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_ContextAction(This,action,result)	\
    ( (This)->lpVtbl -> ContextAction(This,action,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_RequestClose(This)	\
    ( (This)->lpVtbl -> RequestClose(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoWindow_Close(This)	\
    ( (This)->lpVtbl -> Close(This) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CDesktopMode_CGecko_CIGeckoWindow_INTERFACE_DEFINED__ */


/* interface __MIDL_itf_DesktopMode2EGecko_0000_0001 */
/* [local] */ 




extern RPC_IF_HANDLE __MIDL_itf_DesktopMode2EGecko_0000_0001_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_DesktopMode2EGecko_0000_0001_v0_0_s_ifspec;

#ifndef ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_INTERFACE_DEFINED__
#define ____x_ABI_CDesktopMode_CGecko_CIGeckoHost_INTERFACE_DEFINED__

/* interface __x_ABI_CDesktopMode_CGecko_CIGeckoHost */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CDesktopMode_CGecko_CIGeckoHost;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("51a317c4-9633-546c-895e-a7432f947570")
    __x_ABI_CDesktopMode_CGecko_CIGeckoHost : public IInspectable
    {
    public:
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Content( 
            /* [retval][out] */ IInspectable **value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_ExternalContent( 
            /* [retval][out] */ IInspectable **value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsStarted( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsExternalWindowOpen( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Start( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetViewport( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetExternalViewport( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetExternalActive( 
            /* [in] */ boolean active) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE CloseExternalWindow( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetScreen( 
            /* [in] */ double width,
            /* [in] */ double height) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetActive( 
            /* [in] */ boolean active) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE TakeWindowCommand( 
            /* [retval][out] */ int *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE TakeRevealPath( 
            /* [retval][out] */ HSTRING *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE TakeWallpaperPath( 
            /* [retval][out] */ HSTRING *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE OpenUrl( 
            /* [in] */ HSTRING url) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE CreateBrowserWindow( 
            /* [in] */ HSTRING url,
            /* [in] */ boolean appMode,
            /* [retval][out] */ __x_ABI_CDesktopMode_CGecko_CIGeckoWindow **result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE DragFiles( 
            /* [in] */ int state,
            /* [in] */ double x,
            /* [in] */ double y,
            /* [in] */ HSTRING paths,
            /* [retval][out] */ boolean *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Suspend( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Resume( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE Close( void) = 0;
        
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
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, get_ExternalContent)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_ExternalContent )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ IInspectable **value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, get_IsStarted)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsStarted )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ boolean *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, get_IsExternalWindowOpen)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsExternalWindowOpen )( 
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
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, SetExternalViewport)
        HRESULT ( STDMETHODCALLTYPE *SetExternalViewport )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ double width,
            /* [in] */ double height);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, SetExternalActive)
        HRESULT ( STDMETHODCALLTYPE *SetExternalActive )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ boolean active);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, CloseExternalWindow)
        HRESULT ( STDMETHODCALLTYPE *CloseExternalWindow )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, SetScreen)
        HRESULT ( STDMETHODCALLTYPE *SetScreen )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ double width,
            /* [in] */ double height);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, SetActive)
        HRESULT ( STDMETHODCALLTYPE *SetActive )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ boolean active);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, TakeWindowCommand)
        HRESULT ( STDMETHODCALLTYPE *TakeWindowCommand )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ int *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, TakeRevealPath)
        HRESULT ( STDMETHODCALLTYPE *TakeRevealPath )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ HSTRING *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, TakeWallpaperPath)
        HRESULT ( STDMETHODCALLTYPE *TakeWallpaperPath )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [retval][out] */ HSTRING *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, OpenUrl)
        HRESULT ( STDMETHODCALLTYPE *OpenUrl )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ HSTRING url);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, CreateBrowserWindow)
        HRESULT ( STDMETHODCALLTYPE *CreateBrowserWindow )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ HSTRING url,
            /* [in] */ boolean appMode,
            /* [retval][out] */ __x_ABI_CDesktopMode_CGecko_CIGeckoWindow **result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, DragFiles)
        HRESULT ( STDMETHODCALLTYPE *DragFiles )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This,
            /* [in] */ int state,
            /* [in] */ double x,
            /* [in] */ double y,
            /* [in] */ HSTRING paths,
            /* [retval][out] */ boolean *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, Suspend)
        HRESULT ( STDMETHODCALLTYPE *Suspend )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, Resume)
        HRESULT ( STDMETHODCALLTYPE *Resume )( 
            __x_ABI_CDesktopMode_CGecko_CIGeckoHost * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CDesktopMode_CGecko_CIGeckoHost, Close)
        HRESULT ( STDMETHODCALLTYPE *Close )( 
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

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_get_ExternalContent(This,value)	\
    ( (This)->lpVtbl -> get_ExternalContent(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_get_IsStarted(This,value)	\
    ( (This)->lpVtbl -> get_IsStarted(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_get_IsExternalWindowOpen(This,value)	\
    ( (This)->lpVtbl -> get_IsExternalWindowOpen(This,value) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Start(This,width,height)	\
    ( (This)->lpVtbl -> Start(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetViewport(This,width,height)	\
    ( (This)->lpVtbl -> SetViewport(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetExternalViewport(This,width,height)	\
    ( (This)->lpVtbl -> SetExternalViewport(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetExternalActive(This,active)	\
    ( (This)->lpVtbl -> SetExternalActive(This,active) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_CloseExternalWindow(This)	\
    ( (This)->lpVtbl -> CloseExternalWindow(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetScreen(This,width,height)	\
    ( (This)->lpVtbl -> SetScreen(This,width,height) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_SetActive(This,active)	\
    ( (This)->lpVtbl -> SetActive(This,active) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_TakeWindowCommand(This,result)	\
    ( (This)->lpVtbl -> TakeWindowCommand(This,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_TakeRevealPath(This,result)	\
    ( (This)->lpVtbl -> TakeRevealPath(This,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_TakeWallpaperPath(This,result)	\
    ( (This)->lpVtbl -> TakeWallpaperPath(This,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_OpenUrl(This,url)	\
    ( (This)->lpVtbl -> OpenUrl(This,url) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_CreateBrowserWindow(This,url,appMode,result)	\
    ( (This)->lpVtbl -> CreateBrowserWindow(This,url,appMode,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_DragFiles(This,state,x,y,paths,result)	\
    ( (This)->lpVtbl -> DragFiles(This,state,x,y,paths,result) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Suspend(This)	\
    ( (This)->lpVtbl -> Suspend(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Resume(This)	\
    ( (This)->lpVtbl -> Resume(This) ) 

#define __x_ABI_CDesktopMode_CGecko_CIGeckoHost_Close(This)	\
    ( (This)->lpVtbl -> Close(This) ) 

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


