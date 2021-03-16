/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include <stdlib.h>
#include <stdint.h>

#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "MemOperation.h"


void * MemOperation::PinedMemAlloc(uint64_t size)
{
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
    * 
    * Map the memory to an area, without any device/file related.
    * 
    * in Windows: ::VirtualAlloc(0, size,MEM_RESERVE, PAGE_READWRITE);
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
    * 
    * VirtualAlloc(memory, size, MEM_COMMIT, PAGE_READWRITE);
    */
    auto ret = mprotect(memAddress, size, PROT_READ | PROT_WRITE);

    /*
    * 3. Lock
    * 
    * in Windows: VirtualLock(memAddress, size)
    */
    mlock(memAddress, size);

    return memAddress; 
}

void PinedMemFree(void *ptr)
{

}