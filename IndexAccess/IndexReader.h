#ifndef INDEXREADER_H__
#define INDEXREADER_H__

#include <boost/utility.hpp>
#include <boost/intrusive_ptr.hpp>
#include <stdio.h>

class IndexReader: boost::noncopyable
{
    public:
        virtual void GoNext() = 0;
        virtual void GoUntil(uint64_t target, uint64_t limit) = 0;
        virtual bool IsEnd() = 0;
        virtual uint64_t GetDocumentID() = 0;
};

#endif