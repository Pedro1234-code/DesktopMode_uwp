#pragma once
#include "Bridge/GuestConsole.h"
#include "Bridge/GuestThreadpoolTimers.h"
#include <condition_variable>
#include <mutex>
namespace {
BOOL WINAPI FixtureConsoleHandler(DWORD){return TRUE;}
bool ValidateConsoleBufferFixtures(){
    using namespace Win32Bridge::Bridge;
    GuestConsoleContext console;DWORD error=0,done=0;if(!console.Allocate(65001,&error))return false;
    HANDLE output=console.GetStandard(static_cast<DWORD>(-11),&error);
    console.SetSize(8,4);ConsoleBufferInfo info{};
    if(!console.ScreenInfo(output,&info,&error)||info.size.x!=8||info.size.y!=4)return false;
    if(!console.Cursor(output,{3,2},&error)||!console.Attributes(output,0x1e,&error)||!console.ScreenInfo(output,&info,&error)||info.cursor.x!=3||info.cursor.y!=2||info.attributes!=0x1e)return false;
    if(console.Cursor(output,{-1,0},&error)||error!=ERROR_INVALID_PARAMETER)return false;
    if(!console.Fill(output,L'z',false,4,{7,3},&done,&error)||done!=1)return false;
    if(!console.ScreenInfo(output,&info,&error)||info.cursor.x!=3||info.cursor.y!=2)return false;
    auto transcript=console.DrainOutput();if(transcript.find(L"\x1b[3;4H")==std::wstring::npos)return false;
    ConsoleBuffer screen;screen.Resize(8,4);screen.Write(L"ABCDE",5);
    // The same overlap/clip contracts are checked against hidden Windows
    // console buffers in Test-ConsoleBuffer.py; these inspect real bridge cells.
    if(screen.cursor.x!=5||screen.cells[0].character!=L'A')return false;
    if(!console.Attributes(output,7,&error)||!console.Cursor(output,{0,0},&error)||!console.Write(output,L"ABCDE",5,&error)||
       !console.Scroll(output,{0,0,3,0},nullptr,{1,0},{L'_',7},&error))return false;
    transcript=console.DrainOutput();if(transcript.find(L"_ABCD")==std::wstring::npos)return false;
    if(!console.SetControlHandler(&FixtureConsoleHandler,true))return false;
    bool ignored=false;auto handlers=console.ControlHandlers(&ignored);
    if(ignored||handlers.size()!=1||handlers[0]!=&FixtureConsoleHandler)return false;
    if(!console.SetControlHandler(nullptr,true)||!console.SetControlHandler(&FixtureConsoleHandler,false))return false;
    handlers=console.ControlHandlers(&ignored);if(!ignored||!handlers.empty())return false;
    if(!console.FeedInput(std::wstring(1,3)))return false;DWORD code=99;
    return console.TakeControl(&code)&&code==0&&!console.TakeControl(&code);
}
bool ValidateThreadpoolTimerFixtures(){
    using namespace Win32Bridge::Bridge;
    struct State{std::mutex lock;std::condition_variable changed;unsigned count=0;} state;
    GuestThreadpoolTimers timers;
    auto invoke=[&](PTP_TIMER_CALLBACK,void*,PTP_TIMER){std::lock_guard<std::mutex> guard(state.lock);++state.count;state.changed.notify_all();};
    auto callback=+[](PTP_CALLBACK_INSTANCE,PVOID,PTP_TIMER){};
    PTP_TIMER timer=timers.Create(callback,nullptr,invoke);if(!timer)return false;
    LARGE_INTEGER relative{};relative.QuadPart=-10000;FILETIME due{relative.LowPart,static_cast<DWORD>(relative.HighPart)};
    if(!timers.Set(timer,&due,1))return false;
    {std::unique_lock<std::mutex> guard(state.lock);if(!state.changed.wait_for(guard,std::chrono::seconds(2),[&]{return state.count>=2;}))return false;}
    if(!timers.Wait(timer,true))return false;
    unsigned afterWait;{std::lock_guard<std::mutex> guard(state.lock);afterWait=state.count;}
    {std::unique_lock<std::mutex> guard(state.lock);if(!state.changed.wait_for(guard,std::chrono::seconds(2),[&]{return state.count>afterWait;}))return false;}
    if(!timers.Set(timer,nullptr,0)||!timers.Wait(timer,true)||!timers.Close(timer)||timers.Set(timer,&due,0))return false;
    timers.StopAll();unsigned before=state.count;
    if(timers.Create(callback,nullptr,invoke))return false;
    return state.count==before;
}
}
