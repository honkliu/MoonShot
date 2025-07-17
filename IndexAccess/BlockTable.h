#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include <atomic>
#include <thread>
#include <cstdint>
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
            m_rwSpinlock += 2;

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
        ~BlockCache() = default;
        
    
        bool DataLoaded(int block_seq, void** address)
        {
            // TODO: Implement proper cache lookup logic
            *address = nullptr;
            return false;
        }

        void Add(IndexBlock* block, uint32_t block_seq)
        {
            // TODO: Implement cache addition logic
        }
        
    private:
        std::unique_ptr<CacheSlot[]> m_CacheSlots;
        uint32_t m_SlotCount;
};

// Forward declarations
class TermToBlock {
public:
    uint32_t Contains(const char* word) { 
        // TODO: Implement proper term-to-block mapping
        return 0; 
    }
};

class IndexBlockTable
{
    public:
        IndexBlockTable() : m_BlockCache(100)  // Initialize with 100 cache slots
        {
            m_FileManager.reset();
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t number)
        {
            return nullptr;
        }
        
        IndexBlock* GetIndexBlock(const char *word)
        {
            /*
            * Firstly, do element bloom filter
            */
            if (m_ElementFilter && m_ElementFilter->Contains(word)) {
                return nullptr;
            }

            auto block_seq = m_TermToBlock ? m_TermToBlock->Contains(word) : 0;

            auto index_blocks = GetIndexBlock(block_seq, 4);
            
            /*
            * The logic should be:
            * Look up the cache, to find out whether the block_seq is in the cache
            * already. Whether the cache is there or not, the page should be pre-allocated.
            * so find the place to store it
            */
           
            void* block_address = nullptr; 

            if (!m_BlockCache.DataLoaded(block_seq, &block_address))
            {
                if (m_FileManager) {
                    m_FileManager->read(block_seq, block_address);
                }
            }
            m_BlockCache.Add(index_blocks, block_seq);

            return index_blocks;
        }

    private:
        std::shared_ptr<FileBlockManager> m_FileManager;
        std::shared_ptr<TermToBlock> m_TermToBlock;
        std::shared_ptr<ElementFilter> m_ElementFilter;
        BlockCache m_BlockCache;
};

inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;
    return instance;
}

// Constants moved to avoid redefinition issues
constexpr uint64_t MAX_DOCID = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE = 4096;

#endif