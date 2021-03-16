/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef MEMOPERATION_H__
#define MEMOPERATION_H__

#include <stdio.h>

class MemOperation
{
    static void * PinedMemAlloc(uint64_t size);
    static void PinedMemFree(void *ptr);
};

#endif

