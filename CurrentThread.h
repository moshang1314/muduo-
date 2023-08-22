#pragma once 

#include <unistd.h>
#include <sys/syscall.h>

namespace CurrentThread
{
    extern __thread int t_cachedTid;

    void cacheTid();

    inline int tid() {
        //分支优化，表示为假的可能性更大，方便编译器优化
        if (__builtin_expect( t_cachedTid == 0 , 0 )) {
            cacheTid();
        }
        return t_cachedTid;
    }
} // namespace name
