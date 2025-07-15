#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include "FileBlockManager.h"
#include "ElementFilter.h"

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
    struct IndexBlock   *IF_TermToBlock;
};

class RWSpinLock {

    public:
        void ReadLock() 
        {
            m_rwSpinLock += 2;

            while(m_rwSpinlock & 1) {
                /*
                * it is arguable whether the yield is needed. since the IO
                * is fast enough. 
                */
                std::this_thread::yield();
            }

        }

        void ReadUnlock()
        {
            m_rwSpinlock -= 2;
        }

        void WriteLock()
        {
            auto expected = 0;
            while(!m_rwSpinlock.compare_exchange_strong(expected, 1))
            {
                expected = 0;
                std::this_thread::yield();
            }

        }

        void WriteUnlock()
        {
            m_rwSpinlock -= 1;

        }
    private:
        std::atomic<int32_t> m_rwSpinlock;

};

class ReaderSpinLock {

    public:
        ReaderSpinLock(RWSpinLock &p_lock): m_lock(p_lock)
        {
            m_lock.ReadLock();
        }

        ~ReaderSpinLock()
        {
            m_lock.ReadUnlock();
        }

    private:
        RWSpinLock& m_lock;
};

class WriterSpinLock {

    public:
        WriterSpinLock(RWSpinLock &p_lock): m_lock(p_lock)
        {
            m_lock.WriteLock();
        }

        ~WriterSpinLock()
        {
            m_lock.WriteUnlock();
        }

    private:
        RWSpinLock& m_lock;
};

struct CacheSlot {
    RWSpinLock CS_lock;
    uint32_t CS_BlockNum;
};

class BlockCache {
    public:
        BlockCache(uint32_t slot_count):
            m_CacheSlots(new CacheSlot[slot_count])
        {
        }
        ~BlockCache();
        
    
        bool DataLoaded(int block_seq, void * address)
        {
            m_CacheSlots[block_seq % Table]

            address = 
            return false;
        }
        
    private:
        std::unique_ptr<CacheSlot[]> m_CacheSlots;
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
        
        IndexBlock* GetIndexBlock(char *word)
        {
            /*
            * Firstly, do element bloom filter
            */
            if (m_ElementFilter.Contains(word)) {
                return NULL;
            }

            var block_seq = m_TermToBlock.Contains(word);

            var index_blocks = GetIndexBlock(block_seq, 4);
            
            /*
            * The logic should be:
            * Look up the cache, to find out whether the block_seq is in the cache
            * already. Whether the cache is there or not, the page should be pre-allocated.
            * so find the place to store it
            */
           
           void * block_address; 

           if (!m_BlockCache.DataLoaded(block_seq, &block_address))
           {
               m_FileReader.read(block_seq, block_address);



           }
           m_BlockCache.Add(index_blocks, )

        }

    private:
        std::shared_ptr<FileBlockManager> m_FileManager;
        std::shared_ptr<TermToBlock> m_TermToBlock;
        std::shared_ptr<ElementFilter> m_ElementFilter;
};

inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;

    return instance;
}

constexpr uint64_t MAX_DOCID = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE = 4096;

class UnifiedDecoder {
public:
    UnifiedDecoder() : m_block(nullptr), m_current_ptr(nullptr), 
                       m_current_doc(0), m_current_tf(0), 
                       m_block_end(nullptr) {}

    // Open a new index block for decoding
    void Open(IndexBlock* block, uint64_t last_doc_id = 0) {
        m_block = block;
        m_current_doc = last_doc_id;
        m_current_ptr = reinterpret_cast<const uint8_t*>(block->IB_Data);
        m_block_end = m_current_ptr + sizeof(block->IB_Data);
    }

    // Check if at end of block
    bool IsEnd() const {
        return m_current_ptr >= m_block_end || *m_current_ptr == 0;
    }

    // Move to next document
    void GoNext() {
        if (IsEnd()) return;
        
        // Decode delta-compressed document ID
        uint64_t delta = 0;
        uint8_t shift = 0;
        while (true) {
            uint8_t byte = *m_current_ptr++;
            delta |= (byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
        m_current_doc += delta;

        // Decode term frequency
        m_current_tf = 0;
        shift = 0;
        while (true) {
            uint8_t byte = *m_current_ptr++;
            m_current_tf |= (byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
    }

    // Seek to document >= target
    void GoUntil(uint64_t target) {
        while (!IsEnd() && m_current_doc < target) {
            GoNext();
        }
    }

    // Getters
    uint64_t GetDocumentID() const { return m_current_doc; }
    uint32_t GetTermFrequency() const { return m_current_tf; }

private:
    IndexBlock* m_block;            // Current index block
    const uint8_t* m_current_ptr;   // Current read position
    const uint8_t* m_block_end;     // End of block marker
    uint64_t m_current_doc;         // Current document ID
    uint32_t m_current_tf;          // Current term frequency
};

#endif