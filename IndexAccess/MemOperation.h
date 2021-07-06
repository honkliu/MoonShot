/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef MEMOPERATION_H__
#define MEMOPERATION_H__

#include <stdio.h>

void * PinedMemAlloc(uint64_t size);
void PinedMemFree(void *ptr);

#endif

