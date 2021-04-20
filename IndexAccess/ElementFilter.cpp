/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#include <functional>
#include <string>

#include "ElementFilter.h"
void ElementFilter::AddElement(unsigned char *elt)
{
    std::hash<unsigned char *> h_funciton;

    auto n = h(elt)

    m_FilterSpace[n%m_size] = 1;
}
bool ElementFilter::Contains(unsigned char *elt)
{
}


