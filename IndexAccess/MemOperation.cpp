/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include <stdlib.h>
#include <stdint.h>
#include <map>
#include "MemOperation.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

static std::map<void *, uint64_t> gPointerMap;  

void * PinnedMemAlloc(uint64_t size)
{
#ifdef _WIN32
    void *memAddress = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memAddress) {
        return 0;
    }
    VirtualLock(memAddress, size);
    gPointerMap.insert(std::make_pair(memAddress, size));
    return memAddress;
#else
    rlimit res_limit; 
    if (getrlimit(RLIMIT_MEMLOCK, &res_limit) != -1
        && res_limit.rlim_cur != RLIM_INFINITY
        && res_limit.rlim_cur < size) {
        rlimit new_limit = res_limit;
        new_limit.rlim_cur = res_limit.rlim_max == RLIM_INFINITY || res_limit.rlim_max > size
            ? static_cast<rlim_t>(size)
            : res_limit.rlim_max;
        setrlimit(RLIMIT_MEMLOCK, &new_limit);
    }
    void * memAddress = mmap(0, 
                        size, 
                        PROT_READ | PROT_WRITE, 
                        MAP_PRIVATE | MAP_ANONYMOUS, 
                        -1, 
                        0);
    if (memAddress == MAP_FAILED) {
        return 0;
    }
    mlock(memAddress, size);
    gPointerMap.insert(std::make_pair(memAddress, size));
    return memAddress; 
#endif
}

void PinnedMemFree(void *ptr)
{
    auto pointer_pair = gPointerMap.find(ptr);
    if (pointer_pair == gPointerMap.end()) return;
#ifdef _WIN32
    VirtualUnlock(ptr, pointer_pair->second);
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, pointer_pair->second);
#endif
    gPointerMap.erase(pointer_pair);
}