#ifndef INDEXWRITER_H__
#define INDEXWRITER_H__

#include <stdio.h>
#include <vector>
#include <string>
#include <cstdint>

/* 
* An IndexWriter or IndexReader is reprenting a table name
*
* After it is initialized, it will be pointing a table or index name. 
* such that:
*  "T_word": Doc1, Doc2, Doc3
*  "A_word": Doc5, Doc7, Doc9
*  "B_word": Doc1, Doc2, Doc3, Doc4
*
* Then in the future, we could use the IndexReader to read the data, match find the doc
*/

class IndexWriter
{
    public:
        IndexWriter(const IndexWriter&) = delete;

        virtual void Write(std::vector<std::string>&&words, uint64_t documentId, const char * postingType) {}
        //virtual void Write(const std::vector<std::string>& tokens) {};
        //virtual void Close() = {};
        //virtual void Flush() = {};
    protected:
        IndexWriter() = default;
};


#endif