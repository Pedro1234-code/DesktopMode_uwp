"""Hidden-window reference probes for dialog group/default/message semantics.
No bridge implementation is built or executed.
"""
import ctypes as c
from ctypes import wintypes as w
import struct
u=c.WinDLL('user32',use_last_error=True); k=c.WinDLL('kernel32',use_last_error=True)
if c.sizeof(c.c_void_p)!=8: raise SystemExit('Windows x64 required')
k.GetModuleHandleW.argtypes=[w.LPCWSTR]; k.GetModuleHandleW.restype=w.HINSTANCE
u.CreateWindowExW.argtypes=[w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.c_int,c.c_int,c.c_int,c.c_int,w.HWND,w.HMENU,w.HINSTANCE,c.c_void_p]; u.CreateWindowExW.restype=w.HWND
u.DestroyWindow.argtypes=[w.HWND]; u.DestroyWindow.restype=w.BOOL
u.GetNextDlgGroupItem.argtypes=[w.HWND,w.HWND,w.BOOL]; u.GetNextDlgGroupItem.restype=w.HWND
u.EnableWindow.argtypes=[w.HWND,w.BOOL]; u.EnableWindow.restype=w.BOOL
u.SetFocus.argtypes=[w.HWND]; u.SetFocus.restype=w.HWND
u.GetFocus.restype=w.HWND
u.ShowWindow.argtypes=[w.HWND,c.c_int]; u.ShowWindow.restype=w.BOOL
u.SendMessageW.argtypes=[w.HWND,w.UINT,c.c_size_t,c.c_ssize_t]; u.SendMessageW.restype=c.c_ssize_t
u.SetWindowLongPtrW.argtypes=[w.HWND,c.c_int,c.c_ssize_t]; u.SetWindowLongPtrW.restype=c.c_ssize_t
u.IsDialogMessageW.argtypes=[w.HWND,c.POINTER(w.MSG)]; u.IsDialogMessageW.restype=w.BOOL
u.GetWindowTextW.argtypes=[w.HWND,w.LPWSTR,c.c_int]; u.GetWindowTextW.restype=c.c_int
DlgProc=c.WINFUNCTYPE(c.c_ssize_t,w.HWND,w.UINT,c.c_size_t,c.c_ssize_t)
u.CreateDialogIndirectParamW.argtypes=[w.HINSTANCE,c.c_void_p,w.HWND,DlgProc,c.c_ssize_t]; u.CreateDialogIndirectParamW.restype=w.HWND
instance=k.GetModuleHandleW(None); windows=[]; passed=0; events=[]
CHILD,VISIBLE,TAB,GROUP,DISABLED=0x40000000,0x10000000,0x10000,0x20000,0x08000000

def check(value,label):
    global passed
    if not value: raise AssertionError(label)
    passed+=1

def make(parent=None,style=0,kind='STATIC',identifier=0,text='',extended=0):
    hwnd=u.CreateWindowExW(extended,kind,text,style,0,0,100,20,parent,identifier,instance,None)
    if not hwnd: raise c.WinError(c.get_last_error())
    windows.append(hwnd); return hwnd

@DlgProc
def procedure(hwnd,message,wp,lp):
    if message==0x110: return 1
    if message in (0x111,0x8120): events.append((message,wp,lp)); return 1
    return 0

try:
    root=make(style=0x80000000)
    a=make(root,CHILD|VISIBLE|GROUP); b=make(root,CHILD|VISIBLE); hidden=make(root,CHILD)
    d=make(root,CHILD|VISIBLE|GROUP); e=make(root,CHILD|VISIBLE)
    get=u.GetNextDlgGroupItem
    for source,previous,expected in [(None,False,a),(root,False,a),(None,True,None),(root,True,None),
        (a,False,b),(b,False,a),(a,True,b),(b,True,a),(d,False,e),(e,False,d),(d,True,e)]:
        check(get(root,source,previous)==expected,'Group traversal/wrap includes non-tab controls')
    u.EnableWindow(b,False)
    check(get(root,a,False)==a and get(root,b,False)==a,'Disabled entry skipped without leaving group')
    u.EnableWindow(b,True)
    # A hidden WS_GROUP marker still separates groups.
    u.SetWindowLongPtrW(hidden,-16,CHILD|GROUP)
    check(get(root,b,False)==a,'Hidden group marker remains a boundary')
    u.SetWindowLongPtrW(a,-16,CHILD|VISIBLE)
    u.SetWindowLongPtrW(hidden,-16,CHILD)
    check(get(root,e,False)==a and get(root,a,True)==e,'Leading unmarked controls join final circular group')
    # Nested control parents are traversed on entering from a sibling.
    container=make(root,CHILD|VISIBLE,extended=0x10000)
    inner_a=make(container,CHILD|VISIBLE); inner_b=make(container,CHILD|VISIBLE)
    check(get(root,e,False)==inner_a,'Forward traversal enters control-parent')
    u.SetWindowLongPtrW(a,-16,CHILD|VISIBLE|GROUP)
    u.SetWindowLongPtrW(inner_a,-16,CHILD|VISIBLE|GROUP)
    check(get(root,inner_a,False)==inner_b and get(root,inner_b,False)==inner_a,'Nested groups preserve boundaries')
    raw=c.create_string_buffer(struct.pack('<IIHhhhhHHH',0x80000000,0,0,0,0,180,100,0,0,0))
    dialog=u.CreateDialogIndirectParamW(instance,raw,None,procedure,0); check(bool(dialog),'Hidden native dialog')
    windows.append(dialog)
    check(u.SendMessageW(dialog,0x400,0,0)==0x534b0001,'Empty dialog initially retains IDOK')
    default=make(dialog,CHILD|VISIBLE|TAB,kind='BUTTON',identifier=1,text='OK')
    alternate=make(dialog,CHILD|VISIBLE|TAB,kind='BUTTON',identifier=7,text='Other')
    edit=make(dialog,CHILD|VISIBLE|TAB,kind='EDIT',identifier=8)
    checkbox=make(dialog,CHILD|VISIBLE|TAB|3,kind='BUTTON',identifier=9,text='Check')
    check(u.SendMessageW(checkbox,0x87,0,0)==0x2000,'Checkbox does not claim pushbutton/default flags')
    check(u.SendMessageW(edit,0x87,0,0)==0x89,'Single-line edit wants characters/arrows and supports selection')
    multiline=make(dialog,CHILD|VISIBLE|TAB|4,kind='EDIT',identifier=10)
    check(u.SendMessageW(multiline,0x87,0,0)==0x8d,'Multiline edit owns dialog keystrokes')
    for button_type,code,maximum in [(0,0x2020,0),(1,0x2010,0),(2,0x2000,1),(3,0x2000,1),
                                     (4,0x2040,1),(5,0x2000,2),(6,0x2000,2),(7,0x100,0),(9,0x2040,1),(11,0x2000,0)]:
        button=make(root,CHILD|VISIBLE|button_type,kind='BUTTON')
        check(u.SendMessageW(button,0x87,0,0)==code,f'Button dialog flags {button_type}')
        u.SendMessageW(button,0xf1,2,0)
        check(u.SendMessageW(button,0xf0,0,0)==maximum,f'Button check-state limit {button_type}')
    radio=make(root,CHILD|VISIBLE|4,kind='BUTTON')
    u.SendMessageW(radio,0xf1,1,0)
    check(bool(u.SetWindowLongPtrW(radio,-16,CHILD|VISIBLE|4)&TAB),'Checked radio becomes a tab stop')
    u.SendMessageW(radio,0xf1,1,0); u.SendMessageW(radio,0xf1,0,0)
    check(not (u.SetWindowLongPtrW(radio,-16,CHILD|VISIBLE|4)&TAB),'Unchecked radio leaves tab order')
    check(u.SendMessageW(dialog,0x401,1,0)==1,'DM_SETDEFID returns TRUE')
    check(u.SendMessageW(dialog,0x400,0,0)==0x534b0001,'DM_GETDEFID preserves logical default')
    check(u.SendMessageW(default,0x87,0,0)&0x10,'Default button is promoted')
    u.SendMessageW(dialog,0x28,alternate,1)
    check(u.SendMessageW(alternate,0x87,0,0)&0x10,'WM_NEXTDLGCTL temporarily promotes focused pushbutton')
    check(u.SendMessageW(default,0x87,0,0)&0x20,'Previous visual default is demoted')
    check(u.SendMessageW(dialog,0x400,0,0)==0x534b0001,'Focus does not replace logical default ID')
    u.SendMessageW(dialog,0x28,edit,1)
    check(u.SendMessageW(default,0x87,0,0)&0x10,'Moving to edit restores logical default style')
    check(u.SendMessageW(alternate,0x87,0,0)&0x20,'Focused alternate default is demoted after leaving')
    message=w.MSG(); message.hWnd=dialog; message.message=0x8120; message.wParam=123; message.lParam=456
    check(u.IsDialogMessageW(dialog,c.byref(message)) and events[-1]==(0x8120,123,456),'IsDialogMessage dispatches non-key messages')
    message.hWnd=edit; message.message=0x102; message.wParam=ord('x'); message.lParam=1
    check(u.IsDialogMessageW(dialog,c.byref(message)),'Control-owned WM_CHAR still dispatches through IsDialogMessage')
    text=c.create_unicode_buffer(16); u.GetWindowTextW(edit,text,16)
    check(text.value=='x','Edit receives its character exactly once')
    message.hWnd=root; message.message=0x8120
    check(not u.IsDialogMessageW(dialog,c.byref(message)),'Foreign messages remain unhandled')
    u.SendMessageW(dialog,0x28,checkbox,1)
    events.clear(); message.hWnd=checkbox; message.message=0x100; message.wParam=13
    check(u.IsDialogMessageW(dialog,c.byref(message)),'Enter on checkbox handled as a dialog key')
    check(events and events[-1][0]==0x111 and events[-1][1]&0xffff==1,'Enter on checkbox commands the logical default')
    check(u.SendMessageW(checkbox,0xf0,0,0)==0,'Enter does not toggle checkbox')
    u.EnableWindow(default,False); events.clear()
    check(u.IsDialogMessageW(dialog,c.byref(message)) and not events,'Disabled default button suppresses Enter command')
    u.EnableWindow(default,True)
    u.SendMessageW(dialog,0x401,0,0)
    check(u.SendMessageW(dialog,0x400,0,0)==0,'DM_SETDEFID zero clears the default')
    # Hidden focus is legal; only explicit hide/disable transitions clear it.
    hidden_focus=make(root,CHILD,kind='EDIT')
    u.SetFocus(hidden_focus)
    check(u.GetFocus()==hidden_focus,'SetFocus accepts a hidden child of a hidden parent')
    u.ShowWindow(hidden_focus,0)
    check(u.GetFocus()==hidden_focus,'Redundant hide does not clear hidden focus')
    u.ShowWindow(hidden_focus,8)
    check(u.GetFocus()==hidden_focus,'Show without activation preserves focused descendant')
    u.ShowWindow(hidden_focus,0)
    check(u.GetFocus()!=hidden_focus,'A real hide transition removes focus')
    u.SetFocus(hidden_focus); u.EnableWindow(hidden_focus,False)
    check(u.GetFocus()!=hidden_focus,'Disabling the focused child removes focus')
    # Mixed manual/automatic radios share one native dialog group.
    radio_parent=make(root,CHILD|VISIBLE,extended=0x10000)
    manual=make(radio_parent,CHILD|VISIBLE|GROUP|4,kind='BUTTON')
    automatic=make(radio_parent,CHILD|VISIBLE|9,kind='BUTTON')
    u.SendMessageW(manual,0xf1,1,0); u.SendMessageW(automatic,0xf5,0,0)
    check(u.SendMessageW(manual,0xf0,0,0)==0 and u.SendMessageW(automatic,0xf0,0,0)==1,'Auto-radio unchecks a manual radio in the same group')
    u.SendMessageW(automatic,0xf1,0,0); u.EnableWindow(automatic,False)
    u.SendMessageW(automatic,0xf5,0,0)
    check(u.SendMessageW(automatic,0xf0,0,0)==1,'Explicit BM_CLICK acts even on a disabled button')
    # TRUE reevaluates post-WM_INITDIALOG tab order; FALSE preserves explicit
    # focus, even when that control has no WS_VISIBLE bit.
    initialization=[]
    callbacks=[]
    for explicit in (False,True):
        @DlgProc
        def init_procedure(hwnd,message,wp,lp):
            if message==0x110:
                first=make(hwnd,CHILD|VISIBLE|TAB,kind='EDIT')
                second=make(hwnd,CHILD|VISIBLE|TAB,kind='EDIT')
                initialization.append((first,second))
                if explicit:
                    hidden=make(hwnd,CHILD,kind='EDIT'); initialization[-1]+=(hidden,)
                    u.SetFocus(hidden); return 0
                u.EnableWindow(first,False); return 1
            return 0
        callbacks.append(init_procedure)
        created=u.CreateDialogIndirectParamW(instance,raw,None,init_procedure,0)
        windows.append(created)
        check(bool(created),'Initialization contract dialog created')
        expected=initialization[-1][2] if explicit else initialization[-1][1]
        check(u.GetFocus()==expected,'WM_INITDIALOG focus result honors mutation/explicit hidden focus')

finally:
    for hwnd in reversed(windows): u.DestroyWindow(hwnd)
print(f'Passed {passed} native dialog behavior contracts; bridge C++ was not built or run.')
