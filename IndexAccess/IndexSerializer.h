#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include <cstdint>

/*
* Binary index file format (version 1).
*
* Layout:
*   [FileHeader]  64 bytes  — magic, version, section offsets
*   [DocData]     N × 16 B  — per-document id / importance / length
*   [TermDict]    variable  — sorted term keys with posting offsets
*   [Postings]    variable  — VarByte-delta encoded posting lists
*
* Section 0: FileHeader
*   offset  0  char[8]    magic = "MOONSHOT"
*   offset  8  uint32_t   version = 1
*   offset 12  uint32_t   reserved
*   offset 16  uint64_t   num_documents
*   offset 24  uint64_t   num_terms
*   offset 32  uint64_t   docdata_offset
*   offset 40  uint64_t   termdict_offset
*   offset 48  uint64_t   postings_offset
*   offset 56  uint64_t   postings_size
*
* Section 1: DocData  (docdata_offset)
*   per document:
*     uint64_t  doc_id
*     float     importance
*     uint32_t  doc_len
*     uint32_t  padding
*
* Section 2: TermDict  (termdict_offset)
*   per term:
*     uint16_t  key_len
*     char[]    key   (key_len bytes, no null terminator)
*     uint32_t  doc_freq
*     uint64_t  data_offset  (byte offset within Postings section)
*     uint32_t  data_len     (byte count of encoded posting list)
*
* Section 3: Postings  (postings_offset)
*   VarByte-delta-encoded (delta_docid, tf) pairs, packed end-to-end.
*/
class IndexSerializer {
public:
    /*
    * Serialise store to an index file at path.
    * Returns true on success.
    */
    static bool Save(const PostingStore& store, const char* path);

    /*
    * Deserialise an index file into store.
    * Returns true on success.
    */
    static bool Load(PostingStore& store, const char* path);

    /*
    * Quick check: does path begin with the MOONSHOT magic bytes?
    */
    static bool IsValidIndex(const char* path);
};

#endif
