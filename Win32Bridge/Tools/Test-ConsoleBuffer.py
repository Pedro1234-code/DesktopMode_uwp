"""Hidden native console reference contracts; no bridge build/execution."""
import ctypes as c
from ctypes import wintypes as w
import json,subprocess,sys

class Coord(c.Structure): _fields_=[('x',c.c_short),('y',c.c_short)]
class Rect(c.Structure): _fields_=[('left',c.c_short),('top',c.c_short),('right',c.c_short),('bottom',c.c_short)]
class Cell(c.Structure): _fields_=[('char',c.c_wchar),('attributes',w.WORD)]
class Info(c.Structure): _fields_=[('size',Coord),('cursor',Coord),('attributes',w.WORD),('window',Rect),('maximum',Coord)]

def probe():
 k=c.WinDLL('kernel32',use_last_error=True)
 def bind(name,result,*args):
  f=getattr(k,name);f.restype=result;f.argtypes=list(args);return f
 create=bind('CreateConsoleScreenBuffer',w.HANDLE,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,c.c_void_p)
 close=bind('CloseHandle',w.BOOL,w.HANDLE)
 info=bind('GetConsoleScreenBufferInfo',w.BOOL,w.HANDLE,c.POINTER(Info))
 cursor=bind('SetConsoleCursorPosition',w.BOOL,w.HANDLE,Coord)
 attributes=bind('SetConsoleTextAttribute',w.BOOL,w.HANDLE,w.WORD)
 fill=bind('FillConsoleOutputCharacterW',w.BOOL,w.HANDLE,c.c_wchar,w.DWORD,Coord,c.POINTER(w.DWORD))
 fillattr=bind('FillConsoleOutputAttribute',w.BOOL,w.HANDLE,w.WORD,w.DWORD,Coord,c.POINTER(w.DWORD))
 write=bind('WriteConsoleOutputCharacterW',w.BOOL,w.HANDLE,c.c_wchar_p,w.DWORD,Coord,c.POINTER(w.DWORD))
 read=bind('ReadConsoleOutputCharacterW',w.BOOL,w.HANDLE,c.c_void_p,w.DWORD,Coord,c.POINTER(w.DWORD))
 readattr=bind('ReadConsoleOutputAttribute',w.BOOL,w.HANDLE,c.POINTER(w.WORD),w.DWORD,Coord,c.POINTER(w.DWORD))
 scroll=bind('ScrollConsoleScreenBufferW',w.BOOL,w.HANDLE,c.POINTER(Rect),c.POINTER(Rect),Coord,c.POINTER(Cell))
 handle=create(0xc0000000,3,None,1,None)
 assert handle not in (None,c.c_void_p(-1).value),('CreateConsoleScreenBuffer',c.get_last_error())
 count=0
 def check(value):
  nonlocal count
  assert value, c.get_last_error()
  count+=1
 try:
  check(c.sizeof(Info)==22 and c.sizeof(Cell)==4)
  current=Info();check(info(handle,c.byref(current)))
  check(cursor(handle,Coord(3,2)));check(info(handle,c.byref(current)) and (current.cursor.x,current.cursor.y)==(3,2))
  check(not cursor(handle,Coord(-1,0)) and c.get_last_error()==87)
  check(attributes(handle,0x1e));check(info(handle,c.byref(current)) and current.attributes==0x1e)
  done=w.DWORD();check(fill(handle,'x',4,Coord(1,1),c.byref(done)) and done.value==4)
  text=c.create_unicode_buffer(8);check(read(handle,text,4,Coord(1,1),c.byref(done)) and text[:4]=='xxxx')
  check(fillattr(handle,0x2f,4,Coord(1,1),c.byref(done)) and done.value==4)
  values=(w.WORD*4)();check(readattr(handle,values,4,Coord(1,1),c.byref(done)) and list(values)==[0x2f]*4)
  check(info(handle,c.byref(current)) and (current.cursor.x,current.cursor.y)==(3,2))
  check(fill(handle,'z',4,Coord(current.size.x-1,current.size.y-1),c.byref(done)) and done.value==1)
  check(write(handle,'ABCDE',5,Coord(0,0),c.byref(done)))
  area,cell=Rect(0,0,3,0),Cell('_',7)
  check(scroll(handle,c.byref(area),None,Coord(1,0),c.byref(cell)))
  check(read(handle,text,5,Coord(0,0),c.byref(done)) and text[:5]=='_ABCD')
  check(write(handle,'ABCDE',5,Coord(0,0),c.byref(done)))
  clip=Rect(0,0,2,0);check(scroll(handle,c.byref(area),c.byref(clip),Coord(1,0),c.byref(cell)))
  check(read(handle,text,5,Coord(0,0),c.byref(done)) and text[:5]=='_ABDE')
 finally:close(handle)
 return count

if __name__=='__main__':
 if '--child' in sys.argv:print(json.dumps({'checks':probe()}),file=sys.stderr)
 else:
  result=subprocess.run([sys.executable,__file__,'--child'],creationflags=0x08000000,capture_output=True,text=True)
  if result.returncode:raise SystemExit(result.stderr)
  print('PASS:',json.loads(result.stderr)['checks'],'native hidden-console reference checks. Bridge C++ was not compiled/executed.')
