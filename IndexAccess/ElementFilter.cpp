#include <functional>
#include <string>
#include <string_view>

#include "ElementFilter.h"
#include "MemOperation.h"
#include "Constants.h"

ElementFilter::ElementFilter(int size, void * memory)
{
    m_size = size;

    if (memory == NULL) {
        auto pages = size / Constants::PAGE_SIZE;
        auto extra = (size % Constants::PAGE_SIZE) > 0 ? 1 : 0;
        pages += extra;
        m_FilterSpace = (unsigned char *)PinnedMemAlloc(pages * Constants::PAGE_SIZE);
    } else {
        m_FilterSpace = (unsigned char *)memory;
    }
}

ElementFilter::~ElementFilter()
{
    PinnedMemFree((void *)m_FilterSpace);
}

void ElementFilter::AddElement(const char *elt)
{
    if (!m_FilterSpace || m_size <= 0)
        return;

    std::string_view sv(elt);
    std::hash<std::string_view> h;

    auto n1 = h(sv);
    auto n2 = n1 ^ (n1 >> 17);

    m_FilterSpace[n1 % m_size] = 1;
    m_FilterSpace[n2 % m_size] = 1;
}

bool ElementFilter::Contains(const char *elt)
{
    if (!m_FilterSpace || m_size <= 0)
        return true;

    std::string_view sv(elt);
    std::hash<std::string_view> h;

    auto n1 = h(sv);
    auto n2 = n1 ^ (n1 >> 17);

    return m_FilterSpace[n1 % m_size] != 0 &&
           m_FilterSpace[n2 % m_size] != 0;
}
