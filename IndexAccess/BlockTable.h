#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <boost/scoped_ptr.hpp>
#include "FileBlockManager.h"

const int BlockSize = 0x400000;
struct IndexBlock
{
    uint64_t IB_Header;
    unsigned char IB_data[BlockSize];
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
        boost::scoped_ptr<FileBlockManager> m_FileManager;
};

#endif