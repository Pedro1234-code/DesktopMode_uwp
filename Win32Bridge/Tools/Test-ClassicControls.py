"""Native USER32 reference contracts. Does not build or execute bridge C++."""
import ctypes as c
from ctypes import wintypes as w
u=c.WinDLL('user32',use_last_error=True);g=c.WinDLL('gdi32',use_last_error=True)
class Rect(c.Structure):
    _fields_=[('left',w.LONG),('top',w.LONG),('right',w.LONG),('bottom',w.LONG)]
g.CreateCompatibleDC.argtypes=[w.HDC];g.CreateCompatibleDC.restype=w.HDC
g.CreateCompatibleBitmap.argtypes=[w.HDC,c.c_int,c.c_int];g.CreateCompatibleBitmap.restype=w.HBITMAP
g.SelectObject.argtypes=[w.HDC,w.HANDLE];g.SelectObject.restype=w.HANDLE
g.DeleteObject.argtypes=[w.HANDLE];g.DeleteDC.argtypes=[w.HDC]
u.DrawEdge.argtypes=[w.HDC,c.POINTER(Rect),w.UINT,w.UINT];u.DrawEdge.restype=w.BOOL
u.DrawFrameControl.argtypes=[w.HDC,c.POINTER(Rect),w.UINT,w.UINT];u.DrawFrameControl.restype=w.BOOL
u.DrawFocusRect.argtypes=[w.HDC,c.POINTER(Rect)];u.DrawFocusRect.restype=w.BOOL
g.GetPixel.argtypes=[w.HDC,c.c_int,c.c_int];g.GetPixel.restype=w.DWORD
g.SetPixelV.argtypes=[w.HDC,c.c_int,c.c_int,w.DWORD];g.SetPixelV.restype=w.BOOL
u.GetSysColor.argtypes=[c.c_int];u.GetSysColor.restype=w.DWORD
u.GetDC.argtypes=[w.HWND];u.GetDC.restype=w.HDC
u.ReleaseDC.argtypes=[w.HWND,w.HDC]
screen=u.GetDC(None);dc=g.CreateCompatibleDC(screen);bitmap=g.CreateCompatibleBitmap(screen,32,32);u.ReleaseDC(None,screen)
assert dc and bitmap
old=g.SelectObject(dc,bitmap);checks=0
try:
    for edge in [0,1,2,4,8,5,10,6,9]:
        depth=bool(edge&3)+bool(edge&12)
        for sides in [1,2,4,8,15]:
            r=Rect(2,2,30,30)
            assert u.DrawEdge(dc,c.byref(r),edge,sides|0x2000)
            actual=(r.left,r.top,r.right,r.bottom)
            expected=(2+depth*bool(sides&1),2+depth*bool(sides&2),30-depth*bool(sides&4),30-depth*bool(sides&8))
            assert actual==expected,(edge,sides,actual,expected)
            checks+=1
    # Raised/sunken normal edges: sample top and bottom away from corners.
    for edge,colors in [(5,(22,20,21,16)),(10,(16,21,20,22))]:
        r=Rect(2,2,30,30);assert u.DrawEdge(dc,c.byref(r),edge,15)
        for point,index in zip([(10,2),(10,3),(10,29),(10,28)],colors):
            assert g.GetPixel(dc,*point)==u.GetSysColor(index),(edge,point,index)
            checks+=1
    for state in [16,16|0x200,16|0x4000,16|0x100]:
        r=Rect(2,2,30,30);assert u.DrawFrameControl(dc,c.byref(r),4,state|0x2000)
        assert (r.left,r.top,r.right,r.bottom)==(4,4,28,28)
        checks+=1
    for kind,shapes in [(1,[0,1,2,3,4]),(2,[0,1,2,4]),(3,[0,1,2,3,5,8,16]),(4,[0,4,8,16])]:
        for shape in shapes:
            r=Rect(2,2,30,30);assert u.DrawFrameControl(dc,c.byref(r),kind,shape)
            checks+=1
    r=Rect(2,2,30,30)
    for y in range(32):
        for x in range(32):assert g.SetPixelV(dc,x,y,0x73512f)
    assert u.DrawFocusRect(dc,c.byref(r)) and u.DrawFocusRect(dc,c.byref(r))
    assert all(g.GetPixel(dc,x,y)==0x73512f for y in range(32) for x in range(32))
    checks+=1
finally:
    g.SelectObject(dc,old);g.DeleteObject(bitmap);g.DeleteDC(dc)
print(f'PASS: {checks} native classic-control reference contracts. Bridge C++ not built/executed.')
