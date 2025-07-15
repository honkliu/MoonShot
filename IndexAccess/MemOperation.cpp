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

void * PinedMemAlloc(uint64_t size)
{
#ifdef _WIN32
    // Reserve and commit memory
    void *memAddress = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memAddress) {
        return 0;
    }
    // Lock memory
    if (!VirtualLock(memAddress, size)) {
        VirtualFree(memAddress, 0, MEM_RELEASE);
        return 0;
    }
    gPointerMap.insert(std::make_pair(memAddress, size));
    return memAddress;
#else
    /*
    * 0: Set resources
    */
    rlimit res_limit; 
    auto success = getrlimit(RLIMIT_MEMLOCK, &res_limit);
    if(success == -1) {
        return 0;
    }
    res_limit.rlim_cur += size;
    setrlimit(RLIMIT_MEMLOCK, &res_limit);
    /*
    * 1. Reserve
    * Map the memory to an area, without any device/file related.
    */
    void * memAddress = mmap(0, 
                        size, 
                        PROT_NONE, 
                        MAP_PRIVATE | MAP_ANONYMOUS, 
                        -1, 
                        0);
    if (memAddress == MAP_FAILED) {
        return 0;
    }
    /*
    * 2. Commit
    */
    auto ret = mprotect(memAddress, size, PROT_READ | PROT_WRITE);
    /*
    * 3. Lock
    */
    mlock(memAddress, size);
    gPointerMap.insert(std::make_pair(memAddress, size));
    return memAddress; 
#endif
}

void PinedMemFree(void *ptr)
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