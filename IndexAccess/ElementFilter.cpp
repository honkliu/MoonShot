/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#include <functional>
#include <string>

#include "ElementFilter.h"
#include "MemOperation.h"
#include "Constants.h"

ElementFilter::ElementFilter(int size, void * memory)
{
    m_size = size; 

    if (memory == NULL) {
        
        auto pages = size/Constants::PAGE_SIZE;
        auto extra = (size % Constants::PAGE_SIZE) > 0? 1 : 0;

        pages += extra;

        m_FilterSpace = (unsigned char *)PinedMemAlloc(pages * Constants::PAGE_SIZE);
    } else {
        m_FilterSpace = (unsigned char *)memory;
    }
}

ElementFilter::~ElementFilter()
{
    PinedMemFree((void *)m_FilterSpace);
}

void ElementFilter::AddElement(const char *elt)
{
    std::string_view str_view(elt);
    std::hash<std::string_view> h_funciton;

    auto n = h_funciton(str_view);

    m_FilterSpace[n%m_size] = 1;
}
bool ElementFilter::Contains(const char *elt)
{
    return true;
}


