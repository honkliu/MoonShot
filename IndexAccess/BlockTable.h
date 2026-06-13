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

        void resize(uint32_t new_capacity)
        {
            m_Capacity    = new_capacity;
            m_CacheSlots.reset(new CacheSlot[new_capacity]);
            m_Hand        = 0;
            m_PendingSlot = 0;
        }

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


/* SubIndex - sparse in-memory index: one entry per block, sorted by lead term.
 * Mirrors Tiger SubIndexPageRecord.
 * block_entry_start: byte offset in IB_Data where entries start.
 *   0 for normal blocks; 4+cont_len for mixed continuation blocks.
 * page_skip_offset: index into m_PageSkipData (0 = single-block term).
 */
struct SubIndexEntry {
    std::string term;
    uint32_t    block_seq;
    uint32_t    block_entry_start = 0;
    uint32_t    page_skip_offset  = 0;
};

/* IndexBlockTable - page manager + SubIndex. Mirrors Tiger PageManager + WordToPageMap.
 *
 * Continuation block new format (no wasted space):
 *   [CONT_MARKER:2B][cont_len:2B][VarByte:cont_len B][optional new entries...]
 *   New terms after cont data get block_entry_start = 4 + cont_len in SubIndex.
 *
 * PageSkipData: flat vector of uint64_t.
 *   Per multi-block term: [0, base_blk2, base_blk3, ..., UINT64_MAX]
 *   Enables O(log P) GoUntil() cross-block seek.
 */
static constexpr uint16_t BLOCK_CONTINUATION_MARKER = 0xFFFFu;

class IndexBlockTable
{
    public:
        explicit IndexBlockTable(uint32_t cache_capacity = 512)
            : m_BlockCache(cache_capacity)
        {
            m_FileManager.reset();
        }

        void InsertBlock(uint32_t block_seq, const IndexBlock* block)
        {
            void* addr = nullptr;
            m_BlockCache.DataLoaded(static_cast<int>(block_seq), &addr);
            if (!addr) return;
            std::memcpy(addr, block, sizeof(IndexBlock));
            m_BlockCache.Add(static_cast<IndexBlock*>(addr), block_seq);
        }

        void AddSubIndexEntry(const std::string& lead_term, uint32_t block_seq,
                              uint32_t block_entry_start = 0,
                              uint32_t page_skip_offset  = 0)
        {
            m_SubIndex.push_back({lead_term, block_seq, block_entry_start, page_skip_offset});
        }

        void SetSubIndex(std::vector<SubIndexEntry> entries)    { m_SubIndex = std::move(entries); }
        void SetPageSkipData(std::vector<uint64_t>  data)       { m_PageSkipData = std::move(data); }

        const uint64_t* GetPageSkipPtr(uint32_t offset) const
        {
            if (offset == 0 || offset >= m_PageSkipData.size()) return nullptr;
            return m_PageSkipData.data() + offset;
        }

        bool FindTermData(const char* term,
                          uint32_t*   block_seq_out,
                          uint32_t*   offset_out,
                          uint32_t*   data_len_out,
                          uint32_t*   doc_freq_out,
                          bool*       is_last_entry_out    = nullptr,
                          uint32_t*   page_skip_offset_out = nullptr) const
        {
            if (m_SubIndex.empty()) return false;
            auto it = std::upper_bound(m_SubIndex.begin(), m_SubIndex.end(), term,
                [](const char* t, const SubIndexEntry& e) { return t < e.term; });
            if (it == m_SubIndex.begin()) return false;
            --it;

            IndexBlock* block = const_cast<IndexBlockTable*>(this)
                                    ->GetIndexBlock(it->block_seq, 1);
            if (!block) return false;

            bool found = ScanBlock(block, term, it->block_seq, it->block_entry_start,
                                   offset_out, data_len_out, doc_freq_out,
                                   block_seq_out, is_last_entry_out);
            if (found && page_skip_offset_out)
                *page_skip_offset_out = it->page_skip_offset;
            return found;
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t)
        {
            void* address = nullptr;
            bool inCache = m_BlockCache.DataLoaded(static_cast<int>(block_seq), &address);
            if (inCache) return static_cast<IndexBlock*>(address);
            if (m_FileManager && address) {
                m_FileManager->read(block_seq, address);
                m_BlockCache.Add(static_cast<IndexBlock*>(address), block_seq);
                return static_cast<IndexBlock*>(address);
            }
            return nullptr;
        }

        void SetFileManager(std::shared_ptr<FileBlockManager> fm) { m_FileManager = std::move(fm); }
        void ResizeCache(uint32_t capacity) { m_BlockCache.resize(capacity); }

        const std::vector<SubIndexEntry>& GetSubIndex()     const { return m_SubIndex; }
        const std::vector<uint64_t>&      GetPageSkipData() const { return m_PageSkipData; }

    private:
        std::shared_ptr<FileBlockManager> m_FileManager;
        std::shared_ptr<ElementFilter>    m_ElementFilter;
        BlockCache                        m_BlockCache;
        std::vector<SubIndexEntry>        m_SubIndex;
        std::vector<uint64_t>             m_PageSkipData;

        /* Scan from scan_start in IB_Data. scan_start = block_entry_start from SubIndex. */
        static bool ScanBlock(const IndexBlock* block, const char* term,
                              uint32_t block_seq, uint32_t scan_start,
                              uint32_t* offset_out, uint32_t* data_len_out,
                              uint32_t* doc_freq_out, uint32_t* block_seq_out,
                              bool* is_last_entry_out = nullptr)
        {
            const uint8_t* base = block->IB_Data;
            const uint8_t* ptr  = base + scan_start;
            const uint8_t* end  = base + sizeof(block->IB_Data);

            while (ptr + 2 <= end) {
                uint16_t key_len;
                std::memcpy(&key_len, ptr, 2); ptr += 2;
                if (key_len == 0) break;
                if (ptr + key_len + 8 > end) break;

                const char* key = reinterpret_cast<const char*>(ptr);
                ptr += key_len;
                uint32_t doc_freq, data_len;
                std::memcpy(&doc_freq, ptr, 4);
                std::memcpy(&data_len, ptr + 4, 4);
                ptr += 8;

                if (static_cast<size_t>(key_len) == std::strlen(term)
                    && std::memcmp(key, term, key_len) == 0)
                {
                    *block_seq_out = block_seq;
                    *offset_out    = static_cast<uint32_t>(ptr - base);
                    *data_len_out  = data_len;
                    *doc_freq_out  = doc_freq;
                    if (is_last_entry_out) {
                        const uint8_t* next = ptr + data_len;
                        uint16_t nkl = 0;
                        if (next + 2 <= end) std::memcpy(&nkl, next, 2);
                        *is_last_entry_out = (next + 2 > end || nkl == 0);
                    }
                    return true;
                }
                if (ptr + data_len > end) break;
                ptr += data_len;
            }
            return false;
        }
};

/*
* Global singleton — kept for backward compatibility.
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
