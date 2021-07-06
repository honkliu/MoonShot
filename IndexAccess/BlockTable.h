#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include "FileBlockManager.h"

/*
* 4K size, which is the size of 
* a page.
*/
const int BlockSize = 0x1000;
const int NUMBLOCKS = 50; 
struct IndexBlock {
    uint64_t            IB_Header;
    uint32_t            IB_Skip[NUMBLOCKS];
    uint64_t            IB_Data[BlockSize-26];
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