#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace Win32Bridge { namespace Bridge {
struct ConsoleCoord { SHORT x, y; };
struct ConsoleRect { SHORT left, top, right, bottom; };
struct ConsoleCell { wchar_t character; WORD attributes; };
struct ConsoleBufferInfo {
    ConsoleCoord size, cursor; WORD attributes; ConsoleRect window; ConsoleCoord maximum;
};
static_assert(sizeof(ConsoleBufferInfo) == 22, "CONSOLE_SCREEN_BUFFER_INFO ABI");
static_assert(sizeof(ConsoleCell) == 4, "CHAR_INFO ABI");
// Called under the console session lock; no native HWND or console is used.
class ConsoleBuffer {
public:
    SHORT columns = 80, rows = 25; ConsoleCoord cursor{0,0}; WORD attributes = 7;
    std::vector<ConsoleCell> cells = std::vector<ConsoleCell>(80*25, ConsoleCell{L' ',7});
    void Resize(unsigned width, unsigned height) {
        std::vector<ConsoleCell> resized(static_cast<size_t>(width)*height, {L' ',attributes});
        for (unsigned y=0;y<(std::min)(height,static_cast<unsigned>(rows));++y)
            for (unsigned x=0;x<(std::min)(width,static_cast<unsigned>(columns));++x)
                resized[y*width+x]=cells[y*columns+x];
        cells.swap(resized); columns=static_cast<SHORT>(width); rows=static_cast<SHORT>(height);
        cursor.x=(std::min<SHORT>)(cursor.x,columns-1);cursor.y=(std::min<SHORT>)(cursor.y,rows-1);
    }
    bool Valid(ConsoleCoord point) const {return point.x>=0 && point.y>=0 && point.x<columns && point.y<rows;}
    size_t Index(ConsoleCoord point) const {return static_cast<size_t>(point.y)*columns+point.x;}
    ConsoleBufferInfo Info() const {return {{columns,rows},cursor,attributes,{0,0,static_cast<SHORT>(columns-1),static_cast<SHORT>(rows-1)},{columns,rows}};}
    static std::wstring Position(ConsoleCoord point) {return L"\x1b["+std::to_wstring(point.y+1)+L";"+std::to_wstring(point.x+1)+L"H";}
    static std::wstring Color(WORD value) {
        constexpr unsigned ansi[8]={0,4,2,6,1,5,3,7};
        return L"\x1b["+std::to_wstring((value&8?90:30)+ansi[value&7])+L";"+
            std::to_wstring((value&128?100:40)+ansi[(value>>4)&7])+L"m";
    }
    void AdvanceLine() {
        if (++cursor.y<rows)return;
        std::move(cells.begin()+columns,cells.end(),cells.begin());
        std::fill(cells.end()-columns,cells.end(),ConsoleCell{L' ',attributes});cursor.y=rows-1;
    }
    void Write(const wchar_t* text,size_t count) {
        for(size_t i=0;i<count;++i) {
            const wchar_t ch=text[i];
            if(ch==L'\r')cursor.x=0;
            else if(ch==L'\n')AdvanceLine();
            else if(ch==L'\b'){if(cursor.x)--cursor.x;}
            else if(ch==L'\t'){unsigned spaces=8-(cursor.x%8);while(spaces--){const wchar_t space=L' ';Write(&space,1);}}
            else if(ch>=L' '){cells[Index(cursor)]={ch,attributes};if(++cursor.x==columns){cursor.x=0;AdvanceLine();}}
        }
    }
    std::wstring Render(size_t start,size_t count) const {
        std::wstring result=L"\x1b" L"7";
        WORD previous=0xffff;
        for(size_t i=start;i<start+count;++i) {
            if(i==start || i%columns==0)result+=Position({static_cast<SHORT>(i%columns),static_cast<SHORT>(i/columns)});
            if(cells[i].attributes!=previous){previous=cells[i].attributes;result+=Color(previous);}
            result+=cells[i].character>=L' '?cells[i].character:L' ';
        }
        return result+L"\x1b" L"8"+Color(attributes);
    }
};
} }
