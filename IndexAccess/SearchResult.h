#ifndef SEARCHRESULT_H__
#define SEARCHRESULT_H__

#include <cstdint>
#include <string>

struct SearchResult {
    uint64_t    doc_id  = 0;
    float       score   = 0.0f;
    std::string snippet;
};

#endif
