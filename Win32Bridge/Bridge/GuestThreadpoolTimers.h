#pragma once
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Win32Bridge { namespace Bridge {
class GuestThreadpoolTimers final {
    struct Timer {
        std::mutex lock;std::condition_variable changed;
        bool closed=false,armed=false,active=false,exited=false;unsigned generation=0;DWORD period=0;
        std::chrono::steady_clock::time_point due;
        PTP_TIMER_CALLBACK callback;void* context;
        std::function<void(PTP_TIMER_CALLBACK,void*,PTP_TIMER)> invoke;
    };
    std::mutex lock;
    std::unordered_map<Timer*,std::shared_ptr<Timer>> handles;
    std::vector<std::shared_ptr<Timer>> owned;
    bool stopping=false;
    std::shared_ptr<Timer> Find(PTP_TIMER handle){std::lock_guard<std::mutex> guard(lock);auto it=handles.find(reinterpret_cast<Timer*>(handle));return it==handles.end()?nullptr:it->second;}
public:
    ~GuestThreadpoolTimers(){StopAll();}
    PTP_TIMER Create(PTP_TIMER_CALLBACK callback,void* context,std::function<void(PTP_TIMER_CALLBACK,void*,PTP_TIMER)> invoke){
        auto timer=std::make_shared<Timer>();timer->callback=callback;timer->context=context;timer->invoke=std::move(invoke);
        {std::lock_guard<std::mutex> guard(lock);if(stopping)return nullptr;
            owned.erase(std::remove_if(owned.begin(),owned.end(),[](const std::shared_ptr<Timer>& old){std::lock_guard<std::mutex> guard(old->lock);return old->exited;}),owned.end());
            handles.emplace(timer.get(),timer);try{owned.push_back(timer);}catch(...){handles.erase(timer.get());throw;}}
        try{std::thread([timer](){
            std::unique_lock<std::mutex> guard(timer->lock);
            while(!timer->closed){
                if(!timer->armed){timer->changed.wait(guard,[&]{return timer->closed||timer->armed;});continue;}
                auto generation=timer->generation;auto due=timer->due;
                if(timer->changed.wait_until(guard,due,[&]{return timer->closed||!timer->armed||timer->generation!=generation;}))continue;
                timer->active=true;
                if(timer->period)timer->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(timer->period);else timer->armed=false;
                guard.unlock();
                try{timer->invoke(timer->callback,timer->context,reinterpret_cast<PTP_TIMER>(timer.get()));}catch(...){ }
                guard.lock();timer->active=false;timer->changed.notify_all();
            }
            timer->exited=true;guard.unlock();timer->changed.notify_all();
        }).detach();}
        catch(...){
            {std::lock_guard<std::mutex> guard(timer->lock);timer->closed=true;timer->exited=true;timer->changed.notify_all();}
            {std::lock_guard<std::mutex> guard(lock);handles.erase(timer.get());}throw;
        }
        return reinterpret_cast<PTP_TIMER>(timer.get());
    }
    bool Set(PTP_TIMER handle,const FILETIME* due,DWORD period){
        auto timer=Find(handle);if(!timer)return false;
        std::lock_guard<std::mutex> guard(timer->lock);if(timer->closed)return false;
        ++timer->generation;timer->period=period;timer->armed=due!=nullptr;
        if(due){LARGE_INTEGER value{};value.LowPart=due->dwLowDateTime;value.HighPart=due->dwHighDateTime;ULONGLONG ticks;
            if(value.QuadPart<0)ticks=static_cast<ULONGLONG>(-(value.QuadPart+1))+1;
            else {FILETIME now{};::GetSystemTimeAsFileTime(&now);ULARGE_INTEGER current{};current.LowPart=now.dwLowDateTime;current.HighPart=now.dwHighDateTime;ticks=static_cast<ULONGLONG>(value.QuadPart)>current.QuadPart?static_cast<ULONGLONG>(value.QuadPart)-current.QuadPart:0;}
            auto now=std::chrono::steady_clock::now();const auto maxDelay=std::chrono::duration_cast<std::chrono::milliseconds>((std::chrono::steady_clock::time_point::max)()-now).count();
            const auto delay=(std::min)(ticks/10000,static_cast<ULONGLONG>(maxDelay));timer->due=now+std::chrono::milliseconds(delay);
        }
        timer->changed.notify_all();return true;
    }
    // Waiting/canceling pending callbacks must not disarm future timer firings
    // (ReactOS TpWaitForTimer). This serial scheduler has no separate pending queue.
    bool Wait(PTP_TIMER handle,bool cancel){auto timer=Find(handle);if(!timer)return false;(void)cancel;std::unique_lock<std::mutex> guard(timer->lock);timer->changed.wait(guard,[&]{return !timer->active;});return true;}
    bool Close(PTP_TIMER handle){std::shared_ptr<Timer> timer;{std::lock_guard<std::mutex> guard(lock);auto it=handles.find(reinterpret_cast<Timer*>(handle));if(it==handles.end())return false;timer=it->second;handles.erase(it);}std::lock_guard<std::mutex> guard(timer->lock);timer->closed=true;timer->armed=false;timer->changed.notify_all();return true;}
    void StopAll(){std::vector<std::shared_ptr<Timer>> timers;{std::lock_guard<std::mutex> guard(lock);stopping=true;handles.clear();timers.swap(owned);}for(auto& timer:timers){std::lock_guard<std::mutex> guard(timer->lock);timer->closed=true;timer->changed.notify_all();}for(auto& timer:timers){std::unique_lock<std::mutex> guard(timer->lock);timer->changed.wait(guard,[&]{return timer->exited;});}}
};
} }
