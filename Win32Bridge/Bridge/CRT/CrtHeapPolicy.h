#pragma once
#include <cerrno>
#include <cstddef>
#include <limits>
#include <mutex>
namespace Win32Bridge { namespace Bridge {
// UCRT _PNH ABI, not std::new_handler (which has no size argument).
class CrtHeapPolicy final {
public:
    using Handler=int(__cdecl*)(size_t);
    static constexpr size_t MaximumRequest=(std::numeric_limits<size_t>::max)()-31;
    int SetMode(int value){if(value!=0&&value!=1){errno=EINVAL;return -1;}std::lock_guard<std::mutex> guard(m_lock);int previous=m_mode;m_mode=value;return previous;}
    int Mode() const {std::lock_guard<std::mutex> guard(m_lock);return m_mode;}
    Handler SetHandler(Handler handler){std::lock_guard<std::mutex> guard(m_lock);auto previous=m_handler;m_handler=handler;return previous;}
    Handler GetHandler() const {std::lock_guard<std::mutex> guard(m_lock);return m_handler;}
    int CallHandler(size_t size) const {
        // Do not hold a bridge lock while entering guest code. The handler can
        // replace itself, allocate recursively, throw, or leave the guest.
        const auto handler=GetHandler();return handler&&handler(size)!=0?1:0;
    }
    static bool Product(size_t count,size_t size,size_t* total){
        if(count&&size>MaximumRequest/count){errno=ENOMEM;return false;}
        *total=count*size;return true;
    }
    // Aligned CRT blocks retain a pointer header, offset gap and alignment
    // padding. Validate this request before entering the native allocator.
    static int AlignedRequest(size_t size,size_t alignment,size_t offset,size_t* request){
        if(!alignment||(alignment&(alignment-1))||(offset&&offset>=size))return EINVAL;
        const size_t mask=(alignment>sizeof(void*)?alignment:sizeof(void*))-1;
        const size_t gap=(static_cast<size_t>(0)-offset)&(sizeof(void*)-1);
        const size_t fixed=sizeof(void*)+gap;
        if(mask>MaximumRequest-fixed)return ENOMEM;
        const size_t overhead=mask+fixed;
        if(size>MaximumRequest-overhead)return ENOMEM;
        *request=size+overhead;return 0;
    }
    template<class Allocate> void* Retry(size_t size,Allocate allocate) const {
        if(size>MaximumRequest){errno=ENOMEM;return nullptr;}
        const size_t actual=size?size:1;
        for(;;){if(void* memory=allocate(actual))return memory;
            if(!Mode()||!CallHandler(actual)){errno=ENOMEM;return nullptr;}}
    }
private:
    mutable std::mutex m_lock;
    Handler m_handler=nullptr;
    int m_mode=0;
};
} }
