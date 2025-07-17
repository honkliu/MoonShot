#ifndef ADVANCEDINDEXWRITER_H__
#define ADVANCEDINDEXWRITER_H__

#include "IndexWriter.h"
#include <vector>
#include <string>
#include <cstdint>

class AdvancedIndexWriter : public IndexWriter {
public:
    AdvancedIndexWriter() = default;
    virtual ~AdvancedIndexWriter() = default;
    void Write(std::vector<std::string>&& words, uint64_t documentId, const char * postingType);
};

#endif // ADVANCEDINDEXWRITER_H__
