// Classic USER drawing. Geometry is independent of guest executable/class names.
namespace {
bool ClassicValidDc(HDC dc) {
    auto manager=CurrentGuestWindowManager();
    if(!manager||!manager->IsGuestDc(dc)){BridgeSetLastError(ERROR_INVALID_HANDLE);return false;}
    return true;
}
bool ClassicFill(HDC dc, RECT rect, int color) {
    if(rect.right<=rect.left||rect.bottom<=rect.top)return true;
    return BridgeFillRect(dc,&rect,BridgeGetSysColorBrush(color))!=0;
}
void ClassicLine(HDC dc,LONG x,LONG y,LONG endX,LONG endY,int color,LONG thickness=1) {
    const LONG dx=static_cast<LONG>(std::abs(static_cast<long long>(endX)-x));
    const LONG dy=-static_cast<LONG>(std::abs(static_cast<long long>(endY)-y));
    const LONG sx=x<endX?1:-1,sy=y<endY?1:-1;long long error=static_cast<long long>(dx)+dy;
    for(;;){ClassicFill(dc,{x,y,x+thickness,y+thickness},color);if(x==endX&&y==endY)break;
        const auto twice=error*2;if(twice>=dy){error+=dy;x+=sx;}if(twice<=dx){error+=dx;y+=sy;}}
}
void ClassicDisc(HDC dc,RECT box,int color) {
    const LONG width=box.right-box.left,height=box.bottom-box.top;
    if(width<=0||height<=0)return;
    for(LONG y=0;y<height;++y){const double vertical=(2.0*y+1-height)/height;
        const double half=0.5*width*std::sqrt((std::max)(0.0,1.0-vertical*vertical));
        const LONG left=static_cast<LONG>(std::ceil(width*0.5-half-0.5));
        const LONG right=static_cast<LONG>(std::floor(width*0.5+half-0.5))+1;
        ClassicFill(dc,{box.left+left,box.top+y,box.left+right,box.top+y+1},color);}
}
void ClassicInset(RECT& box,LONG amount){box.left+=amount;box.top+=amount;box.right-=amount;box.bottom-=amount;}
void ClassicArrow(HDC dc,RECT box,UINT direction,int color) {
    const LONG cx=box.left+(box.right-box.left)/2,cy=box.top+(box.bottom-box.top)/2;
    const LONG radius=(std::max<LONG>)(1,(std::min)(box.right-box.left,box.bottom-box.top)/4);
    for(LONG row=0;row<=radius;++row){
        if(direction==0)ClassicFill(dc,{cx-row,cy-radius/2+row,cx+row+1,cy-radius/2+row+1},color);
        else if(direction==1)ClassicFill(dc,{cx-row,cy+radius/2-row,cx+row+1,cy+radius/2-row+1},color);
        else if(direction==2)ClassicFill(dc,{cx-radius/2+row,cy-row,cx-radius/2+row+1,cy+row+1},color);
        else ClassicFill(dc,{cx+radius/2-row,cy-row,cx+radius/2-row+1,cy+row+1},color);
    }
}
}

namespace Win32Bridge::Bridge
{
int WINAPI BridgeFrameRect(HDC dc,const RECT* rect,HBRUSH brush) {
    if(!rect){BridgeSetLastError(ERROR_INVALID_PARAMETER);return 0;}
    if(!ClassicValidDc(dc))return 0;
    if(rect->right<=rect->left||rect->bottom<=rect->top)return 1;
    const RECT strips[]={
        {rect->left,rect->top,rect->right,rect->top+1},
        {rect->left,rect->bottom-1,rect->right,rect->bottom},
        {rect->left,rect->top,rect->left+1,rect->bottom},
        {rect->right-1,rect->top,rect->right,rect->bottom}};
    for(const auto& strip:strips)if(!BridgeFillRect(dc,&strip,brush))return 0;
    return 1;
}
BOOL WINAPI BridgeDrawFocusRect(HDC dc,const RECT* rect) {
    if(!rect){BridgeSetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(!ClassicValidDc(dc))return FALSE;
    // XOR, anchored in logical coordinates: drawing twice restores the pixels.
    auto invert=[&](LONG x,LONG y){
        if((static_cast<unsigned long long>(x)+static_cast<unsigned long long>(y))&1)return;
        const COLORREF old=BridgeGetPixel(dc,x,y);
        if(old!=CLR_INVALID)BridgeSetPixelV(dc,x,y,old^0x00ffffffu);
    };
    if(rect->right>rect->left&&rect->bottom>rect->top){
        for(LONG x=rect->left;x<rect->right;++x){invert(x,rect->top);if(rect->bottom-1!=rect->top)invert(x,rect->bottom-1);}
        for(LONG y=rect->top+1;y<rect->bottom-1;++y){invert(rect->left,y);if(rect->right-1!=rect->left)invert(rect->right-1,y);}
    }
    return TRUE;
}
BOOL WINAPI BridgeDrawEdge(HDC dc,LPRECT rect,UINT edge,UINT flags) {
    if(!rect){BridgeSetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(!ClassicValidDc(dc))return FALSE;
    if(flags&BF_DIAGONAL){BridgeSetLastError(ERROR_NOT_SUPPORTED);return FALSE;}
    const UINT outer=edge&3u,inner=edge&12u;
    const bool mono=(flags&BF_MONO)!=0,flat=(flags&BF_FLAT)!=0,soft=(flags&BF_SOFT)!=0;
    const bool valid=(mono||flat)||((outer!=3)&&(inner!=12));
    RECT box=*rect;
    // Inset depth follows the requested outer/inner layers, even when only
    // one side is drawn. BF_ADJUST modifies only the selected sides.
    const int depth=(outer?1:0)+(inner?1:0);
    auto layer=[&](int offset,int topLeft,int bottomRight){
        RECT r=*rect;r.left+=offset;r.top+=offset;r.right-=offset;r.bottom-=offset;
        if(r.right<=r.left||r.bottom<=r.top)return;
        if(flags&BF_TOP)ClassicFill(dc,{r.left,r.top,r.right,r.top+1},topLeft);
        if(flags&BF_LEFT)ClassicFill(dc,{r.left,r.top,r.left+1,r.bottom},topLeft);
        if(flags&BF_BOTTOM)ClassicFill(dc,{r.left,r.bottom-1,r.right,r.bottom},bottomRight);
        if(flags&BF_RIGHT)ClassicFill(dc,{r.right-1,r.top,r.right,r.bottom},bottomRight);
    };
    if(mono||flat){
        if(outer||inner)layer(0,mono?(outer?COLOR_WINDOWFRAME:COLOR_WINDOW):(outer?COLOR_BTNSHADOW:COLOR_BTNFACE),mono?(outer?COLOR_WINDOWFRAME:COLOR_WINDOW):(outer?COLOR_BTNSHADOW:COLOR_BTNFACE));
        if(outer&&inner)layer(1,mono?COLOR_WINDOW:COLOR_BTNFACE,mono?COLOR_WINDOW:COLOR_BTNFACE);
    }else if(valid){
        if(outer){const bool raised=outer==1;layer(0,raised?(soft?COLOR_BTNHIGHLIGHT:COLOR_3DLIGHT):(soft?COLOR_3DDKSHADOW:COLOR_BTNSHADOW),raised?COLOR_3DDKSHADOW:COLOR_BTNHIGHLIGHT);}
        else if(inner)layer(0,inner==4?(soft?COLOR_3DLIGHT:COLOR_BTNHIGHLIGHT):(soft?COLOR_BTNSHADOW:COLOR_3DDKSHADOW),inner==4?COLOR_BTNSHADOW:COLOR_3DLIGHT);
        if(outer&&inner)layer(1,inner==4?(soft?COLOR_3DLIGHT:COLOR_BTNHIGHLIGHT):(soft?COLOR_BTNSHADOW:COLOR_3DDKSHADOW),inner==4?COLOR_BTNSHADOW:COLOR_3DLIGHT);
    }
    if(flags&BF_LEFT)box.left+=depth;if(flags&BF_TOP)box.top+=depth;
    if(flags&BF_RIGHT)box.right-=depth;if(flags&BF_BOTTOM)box.bottom-=depth;
    if((flags&BF_MIDDLE)&&valid)ClassicFill(dc,box,mono?COLOR_WINDOW:COLOR_BTNFACE);
    if(flags&BF_ADJUST)*rect=box;
    return valid?TRUE:FALSE;
}
BOOL WINAPI BridgeDrawFrameControl(HDC dc,LPRECT rect,UINT type,UINT state) {
    if(!rect){BridgeSetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(!ClassicValidDc(dc))return FALSE;
    // DFC_CAPTION=1, MENU=2, SCROLL=3, BUTTON=4. Popup-menu is not a
    // documented classic DrawFrameControl type and must not report success.
    const UINT shape=state&0xffu;
    if(type<1||type>4||(type==1&&shape>4)||(type==2&&shape>4)||
        (type==3&&shape>3&&shape!=5&&shape!=8&&shape!=16)||(type==4&&shape!=0&&shape!=1&&shape!=2&&shape!=4&&shape!=8&&shape!=16)){
        BridgeSetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(type==4&&(shape==1||shape==2)){BridgeSetLastError(ERROR_NOT_SUPPORTED);return FALSE;}
    RECT box=*rect;
    if(box.right<=box.left||box.bottom<=box.top)return TRUE;
    const bool disabled=(state&0x100u)!=0,pushed=(state&0x200u)!=0,checked=(state&0x400u)!=0;
    const bool transparent=(state&0x800u)!=0;
    const int ink=disabled?COLOR_BTNSHADOW:COLOR_BTNTEXT;
    const UINT style=(state&0x8000u?BF_MONO:0)|(state&0x4000u?BF_FLAT:0);
    if(type==2){
        if(!transparent)ClassicFill(dc,box,COLOR_MENU);
    }else if(type==4&&shape!=16){
        const LONG side=(std::min)(box.right-box.left,box.bottom-box.top);
        box.left+=(box.right-box.left-side)/2;box.top+=(box.bottom-box.top-side)/2;box.right=box.left+side;box.bottom=box.top+side;
        if(shape==4){
            ClassicDisc(dc,box,COLOR_BTNSHADOW);
            RECT inside=box;ClassicInset(inside,1);
            ClassicDisc(dc,inside,disabled?COLOR_BTNFACE:COLOR_WINDOW);
            if(checked){ClassicInset(inside,(std::max<LONG>)(1,side/4));ClassicDisc(dc,inside,ink);}
            return TRUE;
        }
        BridgeDrawEdge(dc,&box,EDGE_SUNKEN,BF_RECT|BF_ADJUST|style);
        ClassicFill(dc,box,disabled||pushed?COLOR_BTNFACE:COLOR_WINDOW);
    }else{
        BridgeDrawEdge(dc,&box,(pushed||checked)?EDGE_SUNKEN:EDGE_RAISED,BF_RECT|BF_MIDDLE|BF_ADJUST|BF_SOFT|style);
        if(pushed){++box.left;++box.top;++box.right;++box.bottom;}
    }
    const LONG w=box.right-box.left,h=box.bottom-box.top;
    if(w>0&&h>0){
        if(type==3){if(shape<4||shape==5)ClassicArrow(dc,box,shape==5?1:shape,ink);else{
            ClassicLine(dc,box.right-w/2,box.bottom-1,box.right-1,box.bottom-h/2,ink);
            ClassicLine(dc,box.right-w/3,box.bottom-1,box.right-1,box.bottom-h/3,ink);}}
        else if(type==2){
            if(shape==0||shape==4)ClassicArrow(dc,box,shape==0?3:2,ink);
            else if(shape==2){RECT dot=box;ClassicInset(dot,(std::min)(w,h)/3);ClassicDisc(dc,dot,ink);}
            else if(shape==1||shape==3){ClassicLine(dc,box.left+w/4,box.top+h/2,box.left+w/2,box.top+3*h/4,ink);ClassicLine(dc,box.left+w/2,box.top+3*h/4,box.right-w/5,box.top+h/4,ink);}
            else ClassicArrow(dc,box,0,ink);
        }else if(type==4&&shape!=16&&checked){
            if(shape==8)ClassicFill(dc,{box.left+w/4,box.top+h/3,box.right-w/4,box.bottom-h/3},ink);
            else{const LONG thick=(std::max<LONG>)(1,(std::min)(w,h)/8);ClassicLine(dc,box.left+w/5,box.top+h/2,box.left+2*w/5,box.top+3*h/4,ink,thick);ClassicLine(dc,box.left+2*w/5,box.top+3*h/4,box.right-w/6,box.top+h/4,ink,thick);}
        }else if(type==1){
            RECT glyph=box;ClassicInset(glyph,(std::min)(w,h)/4);
            if(shape==0){ClassicLine(dc,glyph.left,glyph.top,glyph.right-1,glyph.bottom-1,ink);ClassicLine(dc,glyph.left,glyph.bottom-1,glyph.right-1,glyph.top,ink);}
            else if(shape==1)ClassicFill(dc,{glyph.left,glyph.bottom-2,glyph.right,glyph.bottom},ink);
            else if(shape==4){const LONG cx=glyph.left+(glyph.right-glyph.left)/2;ClassicLine(dc,glyph.left,glyph.top,cx,glyph.top,ink);ClassicLine(dc,cx,glyph.top,glyph.right-1,glyph.top+2,ink);ClassicLine(dc,glyph.right-1,glyph.top+2,cx,glyph.top+(glyph.bottom-glyph.top)/2,ink);ClassicFill(dc,{cx,glyph.bottom-1,cx+1,glyph.bottom},ink);}
            else{if(shape==3){RECT rear=glyph;rear.left+=2;rear.bottom-=2;BridgeFrameRect(dc,&rear,BridgeGetSysColorBrush(ink));glyph.top+=2;glyph.right-=2;}BridgeFrameRect(dc,&glyph,BridgeGetSysColorBrush(ink));ClassicFill(dc,{glyph.left,glyph.top,glyph.right,glyph.top+2},ink);}
        }
    }
    if(type==4&&shape==16&&(state&0x2000u))ClassicInset(*rect,2); // DFCS_ADJUSTRECT
    BridgeSetLastError(ERROR_SUCCESS);return TRUE;
}
}
