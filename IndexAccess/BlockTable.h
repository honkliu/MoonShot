#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

const int BlockSize = 0x400000;
struct IndexBlock
{
    uint64_t IB_Header;
    char[BlockSize] IB_data
};

class IndexBlockTable
{
    public:
        BlockTable()
        {
            m_FileManager.reset()
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t number)
        {
            return NULL;
        }
        
    private:
        boost::scoped_ptr<FileBlockManager> m_pFileManager;
};

#endif