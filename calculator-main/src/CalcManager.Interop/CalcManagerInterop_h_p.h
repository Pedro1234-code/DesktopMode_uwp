

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 8.01.0628 */
/* at Tue Jan 19 00:14:07 2038
 */
/* Compiler settings for C:\Users\fcvin\AppData\Local\Temp\CalcManagerInterop.idl-88e315ce:
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

#ifndef __CalcManagerInterop_h_p_h__
#define __CalcManagerInterop_h_p_h__

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

#ifndef ____x_ABI_CCalcManager_CInterop_CIHistoryToken_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIHistoryToken_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIHistoryToken __x_ABI_CCalcManager_CInterop_CIHistoryToken;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIHistoryToken_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISimpleHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISimpleHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CISimpleHandler __x_ABI_CCalcManager_CInterop_CISimpleHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CISimpleHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_FWD_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_FWD_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_FWD_DEFINED__
typedef interface __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory;

#endif 	/* ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_FWD_DEFINED__ */


/* header files for imported files */
#include "inspectable.h"

#ifdef __cplusplus
extern "C"{
#endif 


/* interface __MIDL_itf_CalcManagerInterop_0000_0000 */
/* [local] */ 

















/* [v1_enum] */ 
enum __x_ABI_CCalcManager_CInterop_CCalculatorCommand
    {
        CalculatorCommand_CommandNULL	= 0,
        CalculatorCommand_CommandSIGN	= 80,
        CalculatorCommand_CommandCLEAR	= 81,
        CalculatorCommand_CommandCENTR	= 82,
        CalculatorCommand_CommandBACK	= 83,
        CalculatorCommand_CommandPNT	= 84,
        CalculatorCommand_CommandAnd	= 86,
        CalculatorCommand_CommandOR	= 87,
        CalculatorCommand_CommandXor	= 88,
        CalculatorCommand_CommandLSHF	= 89,
        CalculatorCommand_CommandRSHF	= 90,
        CalculatorCommand_CommandDIV	= 91,
        CalculatorCommand_CommandMUL	= 92,
        CalculatorCommand_CommandADD	= 93,
        CalculatorCommand_CommandSUB	= 94,
        CalculatorCommand_CommandMOD	= 95,
        CalculatorCommand_CommandROOT	= 96,
        CalculatorCommand_CommandPWR	= 97,
        CalculatorCommand_CommandCHOP	= 98,
        CalculatorCommand_CommandROL	= 99,
        CalculatorCommand_CommandROR	= 100,
        CalculatorCommand_CommandCOM	= 101,
        CalculatorCommand_CommandSIN	= 102,
        CalculatorCommand_CommandCOS	= 103,
        CalculatorCommand_CommandTAN	= 104,
        CalculatorCommand_CommandSINH	= 105,
        CalculatorCommand_CommandCOSH	= 106,
        CalculatorCommand_CommandTANH	= 107,
        CalculatorCommand_CommandLN	= 108,
        CalculatorCommand_CommandLOG	= 109,
        CalculatorCommand_CommandSQRT	= 110,
        CalculatorCommand_CommandSQR	= 111,
        CalculatorCommand_CommandCUB	= 112,
        CalculatorCommand_CommandFAC	= 113,
        CalculatorCommand_CommandREC	= 114,
        CalculatorCommand_CommandDMS	= 115,
        CalculatorCommand_CommandCUBEROOT	= 116,
        CalculatorCommand_CommandPOW10	= 117,
        CalculatorCommand_CommandPERCENT	= 118,
        CalculatorCommand_CommandFE	= 119,
        CalculatorCommand_CommandPI	= 120,
        CalculatorCommand_CommandEQU	= 121,
        CalculatorCommand_CommandMCLEAR	= 122,
        CalculatorCommand_CommandRECALL	= 123,
        CalculatorCommand_CommandSTORE	= 124,
        CalculatorCommand_CommandMPLUS	= 125,
        CalculatorCommand_CommandMMINUS	= 126,
        CalculatorCommand_CommandEXP	= 127,
        CalculatorCommand_CommandOPENP	= 128,
        CalculatorCommand_CommandCLOSEP	= 129,
        CalculatorCommand_Command0	= 130,
        CalculatorCommand_Command1	= 131,
        CalculatorCommand_Command2	= 132,
        CalculatorCommand_Command3	= 133,
        CalculatorCommand_Command4	= 134,
        CalculatorCommand_Command5	= 135,
        CalculatorCommand_Command6	= 136,
        CalculatorCommand_Command7	= 137,
        CalculatorCommand_Command8	= 138,
        CalculatorCommand_Command9	= 139,
        CalculatorCommand_CommandA	= 140,
        CalculatorCommand_CommandB	= 141,
        CalculatorCommand_CommandC	= 142,
        CalculatorCommand_CommandD	= 143,
        CalculatorCommand_CommandE	= 144,
        CalculatorCommand_CommandF	= 145,
        CalculatorCommand_CommandINV	= 146,
        CalculatorCommand_CommandSET_RESULT	= 147,
        CalculatorCommand_ModeBasic	= 200,
        CalculatorCommand_ModeScientific	= 201,
        CalculatorCommand_CommandASIN	= 202,
        CalculatorCommand_CommandACOS	= 203,
        CalculatorCommand_CommandATAN	= 204,
        CalculatorCommand_CommandPOWE	= 205,
        CalculatorCommand_CommandASINH	= 206,
        CalculatorCommand_CommandACOSH	= 207,
        CalculatorCommand_CommandATANH	= 208,
        CalculatorCommand_ModeProgrammer	= 209,
        CalculatorCommand_CommandHex	= 313,
        CalculatorCommand_CommandDec	= 314,
        CalculatorCommand_CommandOct	= 315,
        CalculatorCommand_CommandBin	= 316,
        CalculatorCommand_CommandQword	= 317,
        CalculatorCommand_CommandDword	= 318,
        CalculatorCommand_CommandWord	= 319,
        CalculatorCommand_CommandByte	= 320,
        CalculatorCommand_CommandSEC	= 400,
        CalculatorCommand_CommandASEC	= 401,
        CalculatorCommand_CommandCSC	= 402,
        CalculatorCommand_CommandACSC	= 403,
        CalculatorCommand_CommandCOT	= 404,
        CalculatorCommand_CommandACOT	= 405,
        CalculatorCommand_CommandSECH	= 406,
        CalculatorCommand_CommandASECH	= 407,
        CalculatorCommand_CommandCSCH	= 408,
        CalculatorCommand_CommandACSCH	= 409,
        CalculatorCommand_CommandCOTH	= 410,
        CalculatorCommand_CommandACOTH	= 411,
        CalculatorCommand_CommandPOW2	= 412,
        CalculatorCommand_CommandAbs	= 413,
        CalculatorCommand_CommandFloor	= 414,
        CalculatorCommand_CommandCeil	= 415,
        CalculatorCommand_CommandROLC	= 416,
        CalculatorCommand_CommandRORC	= 417,
        CalculatorCommand_CommandLogBaseY	= 500,
        CalculatorCommand_CommandNand	= 501,
        CalculatorCommand_CommandNor	= 502,
        CalculatorCommand_CommandRSHFL	= 505,
        CalculatorCommand_CommandRand	= 600,
        CalculatorCommand_CommandEuler	= 601,
        CalculatorCommand_CommandBINEDITSTART	= 700,
        CalculatorCommand_CommandBINPOS0	= 700,
        CalculatorCommand_CommandBINPOS1	= 701,
        CalculatorCommand_CommandBINPOS2	= 702,
        CalculatorCommand_CommandBINPOS3	= 703,
        CalculatorCommand_CommandBINPOS4	= 704,
        CalculatorCommand_CommandBINPOS5	= 705,
        CalculatorCommand_CommandBINPOS6	= 706,
        CalculatorCommand_CommandBINPOS7	= 707,
        CalculatorCommand_CommandBINPOS8	= 708,
        CalculatorCommand_CommandBINPOS9	= 709,
        CalculatorCommand_CommandBINPOS10	= 710,
        CalculatorCommand_CommandBINPOS11	= 711,
        CalculatorCommand_CommandBINPOS12	= 712,
        CalculatorCommand_CommandBINPOS13	= 713,
        CalculatorCommand_CommandBINPOS14	= 714,
        CalculatorCommand_CommandBINPOS15	= 715,
        CalculatorCommand_CommandBINPOS16	= 716,
        CalculatorCommand_CommandBINPOS17	= 717,
        CalculatorCommand_CommandBINPOS18	= 718,
        CalculatorCommand_CommandBINPOS19	= 719,
        CalculatorCommand_CommandBINPOS20	= 720,
        CalculatorCommand_CommandBINPOS21	= 721,
        CalculatorCommand_CommandBINPOS22	= 722,
        CalculatorCommand_CommandBINPOS23	= 723,
        CalculatorCommand_CommandBINPOS24	= 724,
        CalculatorCommand_CommandBINPOS25	= 725,
        CalculatorCommand_CommandBINPOS26	= 726,
        CalculatorCommand_CommandBINPOS27	= 727,
        CalculatorCommand_CommandBINPOS28	= 728,
        CalculatorCommand_CommandBINPOS29	= 729,
        CalculatorCommand_CommandBINPOS30	= 730,
        CalculatorCommand_CommandBINPOS31	= 731,
        CalculatorCommand_CommandBINPOS32	= 732,
        CalculatorCommand_CommandBINPOS33	= 733,
        CalculatorCommand_CommandBINPOS34	= 734,
        CalculatorCommand_CommandBINPOS35	= 735,
        CalculatorCommand_CommandBINPOS36	= 736,
        CalculatorCommand_CommandBINPOS37	= 737,
        CalculatorCommand_CommandBINPOS38	= 738,
        CalculatorCommand_CommandBINPOS39	= 739,
        CalculatorCommand_CommandBINPOS40	= 740,
        CalculatorCommand_CommandBINPOS41	= 741,
        CalculatorCommand_CommandBINPOS42	= 742,
        CalculatorCommand_CommandBINPOS43	= 743,
        CalculatorCommand_CommandBINPOS44	= 744,
        CalculatorCommand_CommandBINPOS45	= 745,
        CalculatorCommand_CommandBINPOS46	= 746,
        CalculatorCommand_CommandBINPOS47	= 747,
        CalculatorCommand_CommandBINPOS48	= 748,
        CalculatorCommand_CommandBINPOS49	= 749,
        CalculatorCommand_CommandBINPOS50	= 750,
        CalculatorCommand_CommandBINPOS51	= 751,
        CalculatorCommand_CommandBINPOS52	= 752,
        CalculatorCommand_CommandBINPOS53	= 753,
        CalculatorCommand_CommandBINPOS54	= 754,
        CalculatorCommand_CommandBINPOS55	= 755,
        CalculatorCommand_CommandBINPOS56	= 756,
        CalculatorCommand_CommandBINPOS57	= 757,
        CalculatorCommand_CommandBINPOS58	= 758,
        CalculatorCommand_CommandBINPOS59	= 759,
        CalculatorCommand_CommandBINPOS60	= 760,
        CalculatorCommand_CommandBINPOS61	= 761,
        CalculatorCommand_CommandBINPOS62	= 762,
        CalculatorCommand_CommandBINPOS63	= 763,
        CalculatorCommand_CommandBINEDITEND	= 763
    } ;
/* [v1_enum] */ 
enum __x_ABI_CCalcManager_CInterop_CCalculatorMode
    {
        CalculatorMode_Standard	= 0,
        CalculatorMode_Scientific	= 1
    } ;
/* [v1_enum] */ 
enum __x_ABI_CCalcManager_CInterop_CCommandType
    {
        CommandType_UnaryCommand	= 0,
        CommandType_BinaryCommand	= 1,
        CommandType_OperandCommand	= 2,
        CommandType_Parentheses	= 3
    } ;



extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0000_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0000_v0_0_s_ifspec;

#ifndef ____x_ABI_CCalcManager_CInterop_CIHistoryToken_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIHistoryToken_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIHistoryToken */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIHistoryToken;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("1836bc1d-fb63-5a16-a86e-4157515c9041")
    __x_ABI_CCalcManager_CInterop_CIHistoryToken : public IInspectable
    {
    public:
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Value( 
            /* [retval][out] */ HSTRING *value) = 0;
        
        virtual /* [propput] */ HRESULT STDMETHODCALLTYPE put_Value( 
            /* [in] */ HSTRING value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_CommandIndex( 
            /* [retval][out] */ int *value) = 0;
        
        virtual /* [propput] */ HRESULT STDMETHODCALLTYPE put_CommandIndex( 
            /* [in] */ int value) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIHistoryTokenVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryToken, get_Value)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Value )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [retval][out] */ HSTRING *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryToken, put_Value)
        /* [propput] */ HRESULT ( STDMETHODCALLTYPE *put_Value )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [in] */ HSTRING value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryToken, get_CommandIndex)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_CommandIndex )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [retval][out] */ int *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryToken, put_CommandIndex)
        /* [propput] */ HRESULT ( STDMETHODCALLTYPE *put_CommandIndex )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryToken * This,
            /* [in] */ int value);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIHistoryTokenVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIHistoryToken
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIHistoryTokenVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_get_Value(This,value)	\
    ( (This)->lpVtbl -> get_Value(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_put_Value(This,value)	\
    ( (This)->lpVtbl -> put_Value(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_get_CommandIndex(This,value)	\
    ( (This)->lpVtbl -> get_CommandIndex(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryToken_put_CommandIndex(This,value)	\
    ( (This)->lpVtbl -> put_CommandIndex(This,value) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIHistoryToken_INTERFACE_DEFINED__ */


/* interface __MIDL_itf_CalcManagerInterop_0000_0001 */
/* [local] */ 




extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0001_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0001_v0_0_s_ifspec;

#ifndef ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("aa3579ec-a577-518d-9897-986539ade515")
    __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper : public IInspectable
    {
    public:
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Type( 
            /* [retval][out] */ enum __x_ABI_CCalcManager_CInterop_CCommandType *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Command( 
            /* [retval][out] */ int *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Commands( 
            /* [out] */ unsigned int *valueLength,
            /* [out][retval][size_is][size_is] */ int **value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsNegative( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsDecimalPresent( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsSciFmt( 
            /* [retval][out] */ boolean *value) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper, get_Type)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Type )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [retval][out] */ enum __x_ABI_CCalcManager_CInterop_CCommandType *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper, get_Command)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Command )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [retval][out] */ int *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper, get_Commands)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Commands )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [out] */ unsigned int *valueLength,
            /* [out][retval][size_is][size_is] */ int **value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper, get_IsNegative)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsNegative )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [retval][out] */ boolean *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper, get_IsDecimalPresent)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsDecimalPresent )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [retval][out] */ boolean *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper, get_IsSciFmt)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsSciFmt )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper * This,
            /* [retval][out] */ boolean *value);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_get_Type(This,value)	\
    ( (This)->lpVtbl -> get_Type(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_get_Command(This,value)	\
    ( (This)->lpVtbl -> get_Command(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_get_Commands(This,valueLength,value)	\
    ( (This)->lpVtbl -> get_Commands(This,valueLength,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_get_IsNegative(This,value)	\
    ( (This)->lpVtbl -> get_IsNegative(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_get_IsDecimalPresent(This,value)	\
    ( (This)->lpVtbl -> get_IsDecimalPresent(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_get_IsSciFmt(This,value)	\
    ( (This)->lpVtbl -> get_IsSciFmt(This,value) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("294b9ab2-f432-5b52-8e70-5e175a9be648")
    __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory : public IInspectable
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE CreateInstance( 
            /* [in] */ enum __x_ABI_CCalcManager_CInterop_CCommandType type,
            /* [in] */ int command,
            /* [in] */ unsigned int commandsLength,
            /* [in][size_is] */ int *commands,
            /* [in] */ boolean isNegative,
            /* [in] */ boolean isDecimalPresent,
            /* [in] */ boolean isSciFmt,
            /* [out][retval] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper **value) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactoryVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory, CreateInstance)
        HRESULT ( STDMETHODCALLTYPE *CreateInstance )( 
            __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory * This,
            /* [in] */ enum __x_ABI_CCalcManager_CInterop_CCommandType type,
            /* [in] */ int command,
            /* [in] */ unsigned int commandsLength,
            /* [in][size_is] */ int *commands,
            /* [in] */ boolean isNegative,
            /* [in] */ boolean isDecimalPresent,
            /* [in] */ boolean isSciFmt,
            /* [out][retval] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper **value);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactoryVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactoryVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_CreateInstance(This,type,command,commandsLength,commands,isNegative,isDecimalPresent,isSciFmt,value)	\
    ( (This)->lpVtbl -> CreateInstance(This,type,command,commandsLength,commands,isNegative,isDecimalPresent,isSciFmt,value) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapperFactory_INTERFACE_DEFINED__ */


/* interface __MIDL_itf_CalcManagerInterop_0000_0003 */
/* [local] */ 




extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0003_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0003_v0_0_s_ifspec;

#ifndef ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("0e5759f4-4656-56aa-8555-6e1ee58334b1")
    __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper : public IInspectable
    {
    public:
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Tokens( 
            /* [out] */ unsigned int *valueLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryToken ***value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Commands( 
            /* [out] */ unsigned int *valueLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper ***value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Expression( 
            /* [retval][out] */ HSTRING *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_Result( 
            /* [retval][out] */ HSTRING *value) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper, get_Tokens)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Tokens )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [out] */ unsigned int *valueLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryToken ***value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper, get_Commands)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Commands )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [out] */ unsigned int *valueLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper ***value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper, get_Expression)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Expression )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [retval][out] */ HSTRING *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper, get_Result)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_Result )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper * This,
            /* [retval][out] */ HSTRING *value);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_get_Tokens(This,valueLength,value)	\
    ( (This)->lpVtbl -> get_Tokens(This,valueLength,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_get_Commands(This,valueLength,value)	\
    ( (This)->lpVtbl -> get_Commands(This,valueLength,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_get_Expression(This,value)	\
    ( (This)->lpVtbl -> get_Expression(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_get_Result(This,value)	\
    ( (This)->lpVtbl -> get_Result(This,value) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("35721348-0b1c-509c-8d79-d7a8d5549308")
    __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory : public IInspectable
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE CreateInstance( 
            /* [in] */ unsigned int tokensLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryToken **tokens,
            /* [in] */ unsigned int commandsLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper **commands,
            /* [in] */ HSTRING expression,
            /* [in] */ HSTRING result,
            /* [out][retval] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper **value) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactoryVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory, CreateInstance)
        HRESULT ( STDMETHODCALLTYPE *CreateInstance )( 
            __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory * This,
            /* [in] */ unsigned int tokensLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryToken **tokens,
            /* [in] */ unsigned int commandsLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper **commands,
            /* [in] */ HSTRING expression,
            /* [in] */ HSTRING result,
            /* [out][retval] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper **value);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactoryVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactoryVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_CreateInstance(This,tokensLength,tokens,commandsLength,commands,expression,result,value)	\
    ( (This)->lpVtbl -> CreateInstance(This,tokensLength,tokens,commandsLength,commands,expression,result,value) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIHistoryItemWrapperFactory_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("2db30bf1-ef80-557a-afd6-b5db2c56d86a")
    __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ HSTRING display,
            /* [in] */ boolean isError) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler * This,
            /* [in] */ HSTRING display,
            /* [in] */ boolean isError);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_Invoke(This,display,isError)	\
    ( (This)->lpVtbl -> Invoke(This,display,isError) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("4ed519af-6d2a-5bc8-8399-0c3fb2590d3a")
    __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ boolean isError) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler * This,
            /* [in] */ boolean isError);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_Invoke(This,isError)	\
    ( (This)->lpVtbl -> Invoke(This,isError) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("0a14278d-148f-5405-8a4f-196c2083e21b")
    __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ unsigned int tokensLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryToken **tokens,
            /* [in] */ unsigned int commandsLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper **commands) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler * This,
            /* [in] */ unsigned int tokensLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryToken **tokens,
            /* [in] */ unsigned int commandsLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper **commands);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_Invoke(This,tokensLength,tokens,commandsLength,commands)	\
    ( (This)->lpVtbl -> Invoke(This,tokensLength,tokens,commandsLength,commands) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("03583c61-32b3-5caf-82ed-771cd2ac2f44")
    __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ unsigned int count) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler * This,
            /* [in] */ unsigned int count);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_Invoke(This,count)	\
    ( (This)->lpVtbl -> Invoke(This,count) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISimpleHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISimpleHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CISimpleHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CISimpleHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("9b5e7e8e-f415-5af7-97ca-1d70b527e023")
    __x_ABI_CCalcManager_CInterop_CISimpleHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( void) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CISimpleHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CISimpleHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CISimpleHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CISimpleHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CISimpleHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CISimpleHandler * This);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CISimpleHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CISimpleHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CISimpleHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CISimpleHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CISimpleHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CISimpleHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CISimpleHandler_Invoke(This)	\
    ( (This)->lpVtbl -> Invoke(This) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CISimpleHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("721ae5e0-d91e-53a1-87fc-aacdde1aa3a3")
    __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ unsigned int addedItemIndex) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler * This,
            /* [in] */ unsigned int addedItemIndex);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_Invoke(This,addedItemIndex)	\
    ( (This)->lpVtbl -> Invoke(This,addedItemIndex) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("fac7e401-b7f9-5e47-8c06-0ebd1e59ba6e")
    __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ unsigned int memorizedNumbersLength,
            /* [in][size_is] */ HSTRING *memorizedNumbers) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler * This,
            /* [in] */ unsigned int memorizedNumbersLength,
            /* [in][size_is] */ HSTRING *memorizedNumbers);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_Invoke(This,memorizedNumbersLength,memorizedNumbers)	\
    ( (This)->lpVtbl -> Invoke(This,memorizedNumbersLength,memorizedNumbers) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("a3feef78-358c-5ecf-9bbb-5ee5eacead93")
    __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ unsigned int indexOfMemory) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler * This,
            /* [in] */ unsigned int indexOfMemory);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_Invoke(This,indexOfMemory)	\
    ( (This)->lpVtbl -> Invoke(This,indexOfMemory) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("07582ed8-0899-58a1-88cf-8f188c5d3edc")
    __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Invoke( 
            /* [in] */ HSTRING id,
            /* [retval][out] */ HSTRING *result) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandlerVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler, Invoke)
        HRESULT ( STDMETHODCALLTYPE *Invoke )( 
            __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler * This,
            /* [in] */ HSTRING id,
            /* [retval][out] */ HSTRING *result);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandlerVtbl;

    interface __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandlerVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_Invoke(This,id,result)	\
    ( (This)->lpVtbl -> Invoke(This,id,result) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler_INTERFACE_DEFINED__ */


/* interface __MIDL_itf_CalcManagerInterop_0000_0014 */
/* [local] */ 




extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0014_v0_0_c_ifspec;
extern RPC_IF_HANDLE __MIDL_itf_CalcManagerInterop_0000_0014_v0_0_s_ifspec;

#ifndef ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("c48f7858-0bce-588c-aa8d-bcf9b99fc9c9")
    __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper : public IInspectable
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE Reset( 
            /* [in] */ boolean clearMemory) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetStandardMode( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetScientificMode( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetProgrammerMode( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SendCommand( 
            /* [in] */ enum __x_ABI_CCalcManager_CInterop_CCalculatorCommand command) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE MemorizeNumber( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE MemorizedNumberLoad( 
            /* [in] */ unsigned int index) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE MemorizedNumberAdd( 
            /* [in] */ unsigned int index) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE MemorizedNumberSubtract( 
            /* [in] */ unsigned int index) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE MemorizedNumberClear( 
            /* [in] */ unsigned int index) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE MemorizedNumberClearAll( void) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsEngineRecording( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_IsInputEmpty( 
            /* [retval][out] */ boolean *value) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetRadix( 
            /* [in] */ int radixType) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetMemorizedNumbersString( void) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE GetResultForRadix( 
            /* [in] */ unsigned int radix,
            /* [in] */ int precision,
            /* [in] */ boolean groupDigitsPerRadix,
            /* [retval][out] */ HSTRING *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetPrecision( 
            /* [in] */ int precision) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE UpdateMaxIntDigits( void) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_DecimalSeparator( 
            /* [retval][out] */ wchar_t *value) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE GetHistoryItems( 
            /* [out] */ unsigned int *resultLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper ***result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE GetHistoryItemsForMode( 
            /* [in] */ enum __x_ABI_CCalcManager_CInterop_CCalculatorMode mode,
            /* [out] */ unsigned int *resultLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper ***result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetHistoryItems( 
            /* [in] */ unsigned int historyItemsLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper **historyItems) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE GetHistoryItem( 
            /* [in] */ unsigned int index,
            /* [retval][out] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper **result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE RemoveHistoryItem( 
            /* [in] */ unsigned int index,
            /* [retval][out] */ boolean *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE ClearHistory( void) = 0;
        
        virtual /* [propget] */ HRESULT STDMETHODCALLTYPE get_MaxHistorySize( 
            /* [retval][out] */ unsigned __int64 *value) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE GetCurrentDegreeMode( 
            /* [retval][out] */ enum __x_ABI_CCalcManager_CInterop_CCalculatorCommand *result) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetInHistoryItemLoadMode( 
            /* [in] */ boolean isHistoryItemLoadMode) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE GetDisplayCommandsSnapshot( 
            /* [out] */ unsigned int *resultLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper ***result) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, Reset)
        HRESULT ( STDMETHODCALLTYPE *Reset )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ boolean clearMemory);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetStandardMode)
        HRESULT ( STDMETHODCALLTYPE *SetStandardMode )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetScientificMode)
        HRESULT ( STDMETHODCALLTYPE *SetScientificMode )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetProgrammerMode)
        HRESULT ( STDMETHODCALLTYPE *SetProgrammerMode )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SendCommand)
        HRESULT ( STDMETHODCALLTYPE *SendCommand )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ enum __x_ABI_CCalcManager_CInterop_CCalculatorCommand command);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, MemorizeNumber)
        HRESULT ( STDMETHODCALLTYPE *MemorizeNumber )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, MemorizedNumberLoad)
        HRESULT ( STDMETHODCALLTYPE *MemorizedNumberLoad )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int index);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, MemorizedNumberAdd)
        HRESULT ( STDMETHODCALLTYPE *MemorizedNumberAdd )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int index);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, MemorizedNumberSubtract)
        HRESULT ( STDMETHODCALLTYPE *MemorizedNumberSubtract )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int index);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, MemorizedNumberClear)
        HRESULT ( STDMETHODCALLTYPE *MemorizedNumberClear )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int index);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, MemorizedNumberClearAll)
        HRESULT ( STDMETHODCALLTYPE *MemorizedNumberClearAll )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, get_IsEngineRecording)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsEngineRecording )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [retval][out] */ boolean *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, get_IsInputEmpty)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_IsInputEmpty )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [retval][out] */ boolean *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetRadix)
        HRESULT ( STDMETHODCALLTYPE *SetRadix )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ int radixType);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetMemorizedNumbersString)
        HRESULT ( STDMETHODCALLTYPE *SetMemorizedNumbersString )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, GetResultForRadix)
        HRESULT ( STDMETHODCALLTYPE *GetResultForRadix )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int radix,
            /* [in] */ int precision,
            /* [in] */ boolean groupDigitsPerRadix,
            /* [retval][out] */ HSTRING *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetPrecision)
        HRESULT ( STDMETHODCALLTYPE *SetPrecision )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ int precision);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, UpdateMaxIntDigits)
        HRESULT ( STDMETHODCALLTYPE *UpdateMaxIntDigits )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, get_DecimalSeparator)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_DecimalSeparator )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [retval][out] */ wchar_t *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, GetHistoryItems)
        HRESULT ( STDMETHODCALLTYPE *GetHistoryItems )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [out] */ unsigned int *resultLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper ***result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, GetHistoryItemsForMode)
        HRESULT ( STDMETHODCALLTYPE *GetHistoryItemsForMode )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ enum __x_ABI_CCalcManager_CInterop_CCalculatorMode mode,
            /* [out] */ unsigned int *resultLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper ***result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetHistoryItems)
        HRESULT ( STDMETHODCALLTYPE *SetHistoryItems )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int historyItemsLength,
            /* [in][size_is] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper **historyItems);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, GetHistoryItem)
        HRESULT ( STDMETHODCALLTYPE *GetHistoryItem )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int index,
            /* [retval][out] */ __x_ABI_CCalcManager_CInterop_CIHistoryItemWrapper **result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, RemoveHistoryItem)
        HRESULT ( STDMETHODCALLTYPE *RemoveHistoryItem )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ unsigned int index,
            /* [retval][out] */ boolean *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, ClearHistory)
        HRESULT ( STDMETHODCALLTYPE *ClearHistory )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, get_MaxHistorySize)
        /* [propget] */ HRESULT ( STDMETHODCALLTYPE *get_MaxHistorySize )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [retval][out] */ unsigned __int64 *value);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, GetCurrentDegreeMode)
        HRESULT ( STDMETHODCALLTYPE *GetCurrentDegreeMode )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [retval][out] */ enum __x_ABI_CCalcManager_CInterop_CCalculatorCommand *result);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, SetInHistoryItemLoadMode)
        HRESULT ( STDMETHODCALLTYPE *SetInHistoryItemLoadMode )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [in] */ boolean isHistoryItemLoadMode);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper, GetDisplayCommandsSnapshot)
        HRESULT ( STDMETHODCALLTYPE *GetDisplayCommandsSnapshot )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper * This,
            /* [out] */ unsigned int *resultLength,
            /* [out][retval][size_is][size_is] */ __x_ABI_CCalcManager_CInterop_CIExpressionCommandWrapper ***result);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperVtbl;

    interface __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_Reset(This,clearMemory)	\
    ( (This)->lpVtbl -> Reset(This,clearMemory) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetStandardMode(This)	\
    ( (This)->lpVtbl -> SetStandardMode(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetScientificMode(This)	\
    ( (This)->lpVtbl -> SetScientificMode(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetProgrammerMode(This)	\
    ( (This)->lpVtbl -> SetProgrammerMode(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SendCommand(This,command)	\
    ( (This)->lpVtbl -> SendCommand(This,command) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_MemorizeNumber(This)	\
    ( (This)->lpVtbl -> MemorizeNumber(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_MemorizedNumberLoad(This,index)	\
    ( (This)->lpVtbl -> MemorizedNumberLoad(This,index) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_MemorizedNumberAdd(This,index)	\
    ( (This)->lpVtbl -> MemorizedNumberAdd(This,index) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_MemorizedNumberSubtract(This,index)	\
    ( (This)->lpVtbl -> MemorizedNumberSubtract(This,index) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_MemorizedNumberClear(This,index)	\
    ( (This)->lpVtbl -> MemorizedNumberClear(This,index) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_MemorizedNumberClearAll(This)	\
    ( (This)->lpVtbl -> MemorizedNumberClearAll(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_get_IsEngineRecording(This,value)	\
    ( (This)->lpVtbl -> get_IsEngineRecording(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_get_IsInputEmpty(This,value)	\
    ( (This)->lpVtbl -> get_IsInputEmpty(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetRadix(This,radixType)	\
    ( (This)->lpVtbl -> SetRadix(This,radixType) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetMemorizedNumbersString(This)	\
    ( (This)->lpVtbl -> SetMemorizedNumbersString(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetResultForRadix(This,radix,precision,groupDigitsPerRadix,result)	\
    ( (This)->lpVtbl -> GetResultForRadix(This,radix,precision,groupDigitsPerRadix,result) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetPrecision(This,precision)	\
    ( (This)->lpVtbl -> SetPrecision(This,precision) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_UpdateMaxIntDigits(This)	\
    ( (This)->lpVtbl -> UpdateMaxIntDigits(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_get_DecimalSeparator(This,value)	\
    ( (This)->lpVtbl -> get_DecimalSeparator(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetHistoryItems(This,resultLength,result)	\
    ( (This)->lpVtbl -> GetHistoryItems(This,resultLength,result) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetHistoryItemsForMode(This,mode,resultLength,result)	\
    ( (This)->lpVtbl -> GetHistoryItemsForMode(This,mode,resultLength,result) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetHistoryItems(This,historyItemsLength,historyItems)	\
    ( (This)->lpVtbl -> SetHistoryItems(This,historyItemsLength,historyItems) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetHistoryItem(This,index,result)	\
    ( (This)->lpVtbl -> GetHistoryItem(This,index,result) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_RemoveHistoryItem(This,index,result)	\
    ( (This)->lpVtbl -> RemoveHistoryItem(This,index,result) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_ClearHistory(This)	\
    ( (This)->lpVtbl -> ClearHistory(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_get_MaxHistorySize(This,value)	\
    ( (This)->lpVtbl -> get_MaxHistorySize(This,value) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetCurrentDegreeMode(This,result)	\
    ( (This)->lpVtbl -> GetCurrentDegreeMode(This,result) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_SetInHistoryItemLoadMode(This,isHistoryItemLoadMode)	\
    ( (This)->lpVtbl -> SetInHistoryItemLoadMode(This,isHistoryItemLoadMode) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_GetDisplayCommandsSnapshot(This,resultLength,result)	\
    ( (This)->lpVtbl -> GetDisplayCommandsSnapshot(This,resultLength,result) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper_INTERFACE_DEFINED__ */


#ifndef ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_INTERFACE_DEFINED__
#define ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_INTERFACE_DEFINED__

/* interface __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory */
/* [object][uuid] */ 


EXTERN_C const IID IID___x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("85e2c9b7-ef8e-5354-8c34-a4322acc41b9")
    __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory : public IInspectable
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE CreateInstance( 
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler *onSetPrimaryDisplay,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler *onSetIsInError,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler *onSetExpressionDisplay,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler *onSetParenthesisNumber,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onNoRightParenAdded,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onMaxDigitsReached,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onBinaryOperatorReceived,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler *onHistoryItemAdded,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler *onSetMemorizedNumbers,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler *onMemoryItemChanged,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onInputChanged,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler *onGetCEngineString,
            /* [out][retval] */ __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper **value) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactoryVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This);
        
        DECLSPEC_XFGVIRT(IInspectable, GetIids)
        HRESULT ( STDMETHODCALLTYPE *GetIids )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This,
            /* [out] */ ULONG *iidCount,
            /* [size_is][size_is][out] */ IID **iids);
        
        DECLSPEC_XFGVIRT(IInspectable, GetRuntimeClassName)
        HRESULT ( STDMETHODCALLTYPE *GetRuntimeClassName )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This,
            /* [out] */ HSTRING *className);
        
        DECLSPEC_XFGVIRT(IInspectable, GetTrustLevel)
        HRESULT ( STDMETHODCALLTYPE *GetTrustLevel )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This,
            /* [out] */ TrustLevel *trustLevel);
        
        DECLSPEC_XFGVIRT(__x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory, CreateInstance)
        HRESULT ( STDMETHODCALLTYPE *CreateInstance )( 
            __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory * This,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetPrimaryDisplayHandler *onSetPrimaryDisplay,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetIsInErrorHandler *onSetIsInError,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetExpressionDisplayHandler *onSetExpressionDisplay,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetParenthesisNumberHandler *onSetParenthesisNumber,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onNoRightParenAdded,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onMaxDigitsReached,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onBinaryOperatorReceived,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CIOnHistoryItemAddedHandler *onHistoryItemAdded,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISetMemorizedNumbersHandler *onSetMemorizedNumbers,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CIMemoryItemChangedHandler *onMemoryItemChanged,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CISimpleHandler *onInputChanged,
            /* [in] */ __x_ABI_CCalcManager_CInterop_CIGetCEngineStringHandler *onGetCEngineString,
            /* [out][retval] */ __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapper **value);
        
        END_INTERFACE
    } __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactoryVtbl;

    interface __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory
    {
        CONST_VTBL struct __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactoryVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_GetIids(This,iidCount,iids)	\
    ( (This)->lpVtbl -> GetIids(This,iidCount,iids) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_GetRuntimeClassName(This,className)	\
    ( (This)->lpVtbl -> GetRuntimeClassName(This,className) ) 

#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_GetTrustLevel(This,trustLevel)	\
    ( (This)->lpVtbl -> GetTrustLevel(This,trustLevel) ) 


#define __x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_CreateInstance(This,onSetPrimaryDisplay,onSetIsInError,onSetExpressionDisplay,onSetParenthesisNumber,onNoRightParenAdded,onMaxDigitsReached,onBinaryOperatorReceived,onHistoryItemAdded,onSetMemorizedNumbers,onMemoryItemChanged,onInputChanged,onGetCEngineString,value)	\
    ( (This)->lpVtbl -> CreateInstance(This,onSetPrimaryDisplay,onSetIsInError,onSetExpressionDisplay,onSetParenthesisNumber,onNoRightParenAdded,onMaxDigitsReached,onBinaryOperatorReceived,onHistoryItemAdded,onSetMemorizedNumbers,onMemoryItemChanged,onInputChanged,onGetCEngineString,value) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* ____x_ABI_CCalcManager_CInterop_CICalculatorManagerWrapperFactory_INTERFACE_DEFINED__ */


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


