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
* PAGE_SIZE — one IndexBlock maps to one 4 KB physical page.
*/
static constexpr int PAGE_SIZE  = 4096;
static constexpr int NUMBLOCKS  = 50;

/*
* IB_Header bit layout:
*   bit  63     : HAS_MORE — posting list continues in block_seq + 1
*   bits 62..0  : block sequence number
*
*   IB_Skip  : 200 bytes — future skip-list entries (zero for now)
*   IB_Data  : 3888 bytes — VarByte-delta-encoded (docId, tf) pairs
*                           followed by a zero sentinel byte
*/

static constexpr uint64_t IB_HEADER_HAS_MORE = (1ULL << 63);

struct IndexBlock {
    uint64_t IB_Header;
    uint32_t IB_Skip[NUMBLOCKS];
    uint8_t  IB_Data[PAGE_SIZE
                     - static_cast<int>(sizeof(uint64_t))
                     - NUMBLOCKS * static_cast<int>(sizeof(uint32_t))];
};
static_assert(sizeof(IndexBlock) == PAGE_SIZE, "IndexBlock must be exactly PAGE_SIZE bytes");

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

struct CacheSlot {
    RWSpinLock  CS_lock;
    uint32_t    CS_BlockNum  = UINT32_MAX;  // UINT32_MAX = empty
    bool        CS_Valid     = false;
    bool        CS_Touched   = false;
    IndexBlock  CS_Data;
};

/*
* BlockCache — fixed-capacity buffer pool with clock-based eviction.
*
* DataLoaded(seq, &addr):
*   Hit  — sets addr to the cached block, returns true.
*   Miss — claims a victim slot, sets addr to that slot's buffer
*          so the caller can fill it, returns false.
*
* Add(block, seq):
*   Marks the slot prepared by DataLoaded as valid.
*/
class BlockCache {
    public:
        explicit BlockCache(uint32_t slot_count)
            : m_Capacity(slot_count)
            , m_CacheSlots(new CacheSlot[slot_count])
            , m_Hand(0)
        {}

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
            if (!block)
                return;

            for (uint32_t i = 0; i < m_Capacity; ++i) {
                if (m_CacheSlots[i].CS_BlockNum == block_seq &&
                    !m_CacheSlots[i].CS_Valid)
                {
                    WriterSpinLock guard(m_CacheSlots[i].CS_lock);
                    if (block != &m_CacheSlots[i].CS_Data)
                        m_CacheSlots[i].CS_Data = *block;

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

            uint32_t victim = m_Hand;
            m_Hand = (m_Hand + 1) % m_Capacity;
            return victim;
        }
};

/*
* TermToBlock — maps a term+stream key (e.g. "rustT") to the block
* sequence number of the first block holding its posting list.
* Populated by IndexContext::Build() during the write→read transition.
*/
class TermToBlock {
public:
    /*
    * Returns UINT32_MAX when the term is not in the index.
    */
    uint32_t Contains(const char* word) const
    {
        auto it = m_Map.find(word);
        return it != m_Map.end() ? it->second : UINT32_MAX;
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

/*
* IndexBlockTable — the page manager for posting-list blocks.
* Equivalent to REF's PageManager + HashCacheProxy.
*
* GetIndexBlock(word):
*   1. Lookup TermToBlock for the block sequence number.
*   2. Check BlockCache (clock replacement).
*   3. On cache miss: load from FileBlockManager (disk).
*
* InsertBlock(seq, bytes, len):
*   Write path — stores a VarByte-encoded posting list directly
*   into the cache.  Used by IndexContext::Build().
*
* AddTermMapping(term, seq):
*   Register a term → first-block mapping during Build().
*/
class IndexBlockTable
{
    public:
        explicit IndexBlockTable(uint32_t cache_capacity = 512)
            : m_TermToBlock(std::make_shared<TermToBlock>())
            , m_BlockCache(cache_capacity)
        {
            m_FileManager.reset();
        }

        /*
        * Store a VarByte posting list directly into the block cache.
        * hasMore = true means this posting list continues in block_seq + 1.
        */
        void InsertBlock(uint32_t    block_seq,
                         const uint8_t* data,
                         size_t         data_len,
                         bool           hasMore = false)
        {
            void* addr = nullptr;
            m_BlockCache.DataLoaded(static_cast<int>(block_seq), &addr);

            if (!addr)
                return;

            IndexBlock* block = static_cast<IndexBlock*>(addr);

            block->IB_Header = static_cast<uint64_t>(block_seq);
            if (hasMore)
                block->IB_Header |= IB_HEADER_HAS_MORE;

            std::memset(block->IB_Skip, 0, sizeof(block->IB_Skip));
            std::memset(block->IB_Data, 0, sizeof(block->IB_Data));

            size_t copyLen = std::min(data_len, sizeof(block->IB_Data) - 1u);
            std::memcpy(block->IB_Data, data, copyLen);

            m_BlockCache.Add(block, block_seq);
        }

        /*
        * Register term → first block mapping (called during Build()).
        */
        void AddTermMapping(const std::string& term, uint32_t block_seq)
        {
            m_TermToBlock->AddMapping(term, block_seq);
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t /*number*/)
        {
            void* address = nullptr;
            bool  inCache = m_BlockCache.DataLoaded(
                                static_cast<int>(block_seq), &address);

            if (inCache) {
                /*
                * Cache hit — block is already in memory.
                */
                return static_cast<IndexBlock*>(address);
            }

            /*
            * Cache miss.  Load from disk if a FileManager is configured.
            * Without a FileManager (pure in-memory index) the block does
            * not exist — return nullptr so callers treat it as absent.
            */
            if (m_FileManager && address) {
                m_FileManager->read(block_seq, address);
                m_BlockCache.Add(static_cast<IndexBlock*>(address), block_seq);
                return static_cast<IndexBlock*>(address);
            }

            return nullptr;
        }

        IndexBlock* GetIndexBlock(const char* word)
        {
            if (m_ElementFilter && m_ElementFilter->Contains(word))
                return nullptr;

            uint32_t block_seq = m_TermToBlock
                                     ? m_TermToBlock->Contains(word)
                                     : UINT32_MAX;

            if (block_seq == UINT32_MAX)
                return nullptr;

            return GetIndexBlock(block_seq, 1);
        }

        bool HasTerm(const std::string& term) const
        {
            return m_TermToBlock && m_TermToBlock->HasTerm(term);
        }

    private:
        std::shared_ptr<FileBlockManager> m_FileManager;
        std::shared_ptr<TermToBlock>      m_TermToBlock;
        std::shared_ptr<ElementFilter>    m_ElementFilter;
        BlockCache                        m_BlockCache;
};

/*
* Global singleton — kept for AdvancedIndexReader backward compatibility.
* New code should use the IndexBlockTable owned by IndexContext.
*/
inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;
    return instance;
}

constexpr uint64_t MAX_DOCID       = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE  = PAGE_SIZE;

#endif
