#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include "FileBlockManager.h"

const int BlockSize = 0x400000;

struct IndexBlock {
    uint64_t            IB_Header;
    unsigned char       IB_data[BlockSize];
};

struct IndexFile {
    uint64_t *          IF_Header;
    struct IndexBlock   *IF_DocData;
    struct IndexBlock   *IF_DocSkipData;
    struct IndexBlock   *IF_Data;
};

class IndexBlockTable
{
    public:
        IndexBlockTable()
        {
            m_FileManager.reset();
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t number)
        {
            return NULL;
        }
        
    private:
        std::shared_ptr<FileBlockManager> m_FileManager;
};

#endif