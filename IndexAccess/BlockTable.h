#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include "FileBlockManager.h"
#include "ElementFilter.h"

/*
* 4K size, which is the size of a page.
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
        std::atomic<int32_t> m_rwSpinlock {0};

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

/*
* Each slot in the block cache holds one IndexBlock and tracks
* its block sequence number, validity, and a clock "touched" bit.
*/
struct CacheSlot {
    RWSpinLock  CS_lock;
    uint32_t    CS_BlockNum = UINT32_MAX;   // UINT32_MAX means empty
    bool        CS_Valid    = false;
    bool        CS_Touched  = false;
    IndexBlock  CS_Data;
};

/*
* Fixed-capacity buffer pool with clock-based eviction.
*
* DataLoaded(seq, &addr):
*   Hit  — sets addr to the cached block, returns true.
*   Miss — claims a victim slot (evicting if full), sets addr to that
*          slot's buffer so the caller can fill it, returns false.
*
* Add(block, seq):
*   Marks the slot that DataLoaded prepared as valid.
*   If block != the slot's own buffer, copies the data in.
*/
class BlockCache {
    public:
        explicit BlockCache(uint32_t slot_count)
            : m_Capacity(slot_count)
            , m_CacheSlots(new CacheSlot[slot_count])
            , m_Hand(0)
        {
        }

        ~BlockCache() = default;

        bool DataLoaded(int block_seq, void** address)
        {
            uint32_t seq = static_cast<uint32_t>(block_seq);

            for (uint32_t i = 0; i < m_Capacity; ++i) {
                ReaderSpinLock guard(m_CacheSlots[i].CS_lock);
                if (m_CacheSlots[i].CS_Valid &&
                    m_CacheSlots[i].CS_BlockNum == seq)
                {
                    m_CacheSlots[i].CS_Touched = true;
                    *address = &m_CacheSlots[i].CS_Data;
                    return true;
                }
            }

            /*
            * Miss — pick a victim slot using the clock algorithm and
            * hand its buffer to the caller for filling.
            */
            uint32_t victim = PickVictim();
            {
                WriterSpinLock guard(m_CacheSlots[victim].CS_lock);
                m_CacheSlots[victim].CS_BlockNum = seq;
                m_CacheSlots[victim].CS_Valid    = false;
                m_CacheSlots[victim].CS_Touched  = false;
            }
            *address = &m_CacheSlots[victim].CS_Data;
            m_PendingSlot = victim;
            return false;
        }

        void Add(IndexBlock* block, uint32_t block_seq)
        {
            if (!block) return;

            /*
            * Find the slot we prepared in DataLoaded and mark it valid.
            * Copy data only if the caller supplied a different buffer.
            */
            for (uint32_t i = 0; i < m_Capacity; ++i) {
                if (m_CacheSlots[i].CS_BlockNum == block_seq &&
                    !m_CacheSlots[i].CS_Valid)
                {
                    WriterSpinLock guard(m_CacheSlots[i].CS_lock);
                    if (block != &m_CacheSlots[i].CS_Data) {
                        m_CacheSlots[i].CS_Data = *block;
                    }
                    m_CacheSlots[i].CS_Valid   = true;
                    m_CacheSlots[i].CS_Touched = true;
                    return;
                }
            }
        }

    private:
        uint32_t                     m_Capacity;
        std::unique_ptr<CacheSlot[]> m_CacheSlots;
        uint32_t                     m_Hand;
        uint32_t                     m_PendingSlot = 0;

        uint32_t PickVictim()
        {
            /*
            * Clock sweep: give touched slots a second chance by clearing
            * the bit; evict the first untouched (or empty) slot found.
            */
            for (uint32_t i = 0; i < m_Capacity * 2; ++i) {
                uint32_t candidate = m_Hand;
                m_Hand = (m_Hand + 1) % m_Capacity;

                if (!m_CacheSlots[candidate].CS_Valid)
                    return candidate;

                if (!m_CacheSlots[candidate].CS_Touched) {
                    m_CacheSlots[candidate].CS_Valid = false;
                    return candidate;
                }

                m_CacheSlots[candidate].CS_Touched = false;
            }

            // All slots are pinned/touched — evict the hand anyway.
            uint32_t victim = m_Hand;
            m_Hand = (m_Hand + 1) % m_Capacity;
            return victim;
        }
};

/*
* Maps a term string to the block sequence number that holds its posting list.
* Populated by the index writer; queried by IndexBlockTable at search time.
*/
class TermToBlock {
public:
    uint32_t Contains(const char* word)
    {
        auto it = m_Map.find(word);
        return it != m_Map.end() ? it->second : 0;
    }

    void AddMapping(const std::string& term, uint32_t block_seq)
    {
        m_Map[term] = block_seq;
    }

    bool HasTerm(const std::string& term) const
    {
        return m_Map.find(term) != m_Map.end();
    }

private:
    std::unordered_map<std::string, uint32_t> m_Map;
};

class IndexBlockTable
{
    public:
        IndexBlockTable() : m_BlockCache(100)
        {
            m_FileManager.reset();
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t /*number*/)
        {
            void* address = nullptr;
            if (!m_BlockCache.DataLoaded(static_cast<int>(block_seq), &address))
            {
                if (m_FileManager && address) {
                    m_FileManager->read(block_seq, address);
                }
            }
            if (address) {
                m_BlockCache.Add(static_cast<IndexBlock*>(address), block_seq);
            }
            return static_cast<IndexBlock*>(address);
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

            return GetIndexBlock(block_seq, 1);
        }

    private:
        std::shared_ptr<FileBlockManager> m_FileManager;
        std::shared_ptr<TermToBlock>      m_TermToBlock;
        std::shared_ptr<ElementFilter>    m_ElementFilter;
        BlockCache                        m_BlockCache;
};

inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;
    return instance;
}

constexpr uint64_t MAX_DOCID = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE = 4096;

#endif
