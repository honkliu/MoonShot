#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef PAGE_SIZE
#undef PAGE_SIZE
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "FileBlockManager.h"
#include "ElementFilter.h"

static constexpr int PAGE_SIZE  = 4096;
static constexpr int NUMBLOCKS  = 50;

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

/*
* BloomFilter — placeholder (Tiger uses a Bloom filter to reject absent terms quickly).
* Always returns true so lookups fall through to the TermHeaderTable.
*/
struct BloomFilter {
    bool CanTermExist(const char* /*term*/, size_t /*len*/) const { return true; }
};

class RWSpinLock {
    public:
        void ReadLock()  { m_rwSpinlock += 2; while(m_rwSpinlock & 1) std::this_thread::yield(); }
        void ReadUnlock(){ m_rwSpinlock -= 2; }
        void WriteLock() {
            auto expected = 0;
            while(!m_rwSpinlock.compare_exchange_strong(expected, 1)) { expected = 0; std::this_thread::yield(); }
        }
        void WriteUnlock() { m_rwSpinlock -= 1; }
    private:
        std::atomic<int32_t> m_rwSpinlock {0};
};

class ReaderSpinLock {
    public:
        ReaderSpinLock(RWSpinLock &p_lock): m_lock(p_lock) { m_lock.ReadLock(); }
        ~ReaderSpinLock() { m_lock.ReadUnlock(); }
    private:
        RWSpinLock& m_lock;
};

class WriterSpinLock {
    public:
        WriterSpinLock(RWSpinLock &p_lock): m_lock(p_lock) { m_lock.WriteLock(); }
        ~WriterSpinLock() { m_lock.WriteUnlock(); }
    private:
        RWSpinLock& m_lock;
};

struct CacheSlot {
    RWSpinLock  CS_lock;
    uint32_t    CS_BlockNum  = UINT32_MAX;
    bool        CS_Valid     = false;
    bool        CS_Touched   = false;
    IndexBlock  CS_Data;
};

static inline bool PinMemoryPages(void* address, size_t bytes)
{
    if (!address || bytes == 0) return false;
#ifdef _WIN32
    SIZE_T minWorkingSet = 0, maxWorkingSet = 0;
    HANDLE process = GetCurrentProcess();
    if (GetProcessWorkingSetSize(process, &minWorkingSet, &maxWorkingSet)) {
        SIZE_T desired = bytes + (16ull * 1024ull * 1024ull);
        if (maxWorkingSet < desired)
            SetProcessWorkingSetSize(process, minWorkingSet, desired);
    }
    return VirtualLock(address, bytes) != 0;
#else
    return mlock(address, bytes) == 0;
#endif
}

static inline void UnpinMemoryPages(void* address, size_t bytes)
{
    if (!address || bytes == 0) return;
#ifdef _WIN32
    VirtualUnlock(address, bytes);
#else
    munlock(address, bytes);
#endif
}

class BlockCache {
    public:
        explicit BlockCache(uint32_t slot_count)
            : m_Capacity(std::max<uint32_t>(slot_count, 1)),
              m_CacheSlots(new CacheSlot[std::max<uint32_t>(slot_count, 1)]),
              m_Hand(0)
        {
            m_PinBytes = sizeof(CacheSlot) * static_cast<size_t>(m_Capacity);
            m_IsPinned = PinMemoryPages(m_CacheSlots.get(), m_PinBytes);
        }
        ~BlockCache() { UnpinMemoryPages(m_CacheSlots.get(), m_PinBytes); }

        void resize(uint32_t new_capacity) {
            new_capacity = std::max<uint32_t>(new_capacity, 1);
            UnpinMemoryPages(m_CacheSlots.get(), m_PinBytes);
            m_Capacity    = new_capacity;
            m_CacheSlots.reset(new CacheSlot[new_capacity]);
            m_BlockToSlot.clear();
            m_Hand = m_PendingSlot = 0;
            m_PinBytes = sizeof(CacheSlot) * static_cast<size_t>(m_Capacity);
            m_IsPinned = PinMemoryPages(m_CacheSlots.get(), m_PinBytes);
        }

        bool DataLoaded(int block_seq, void** address) {
            uint32_t seq = static_cast<uint32_t>(block_seq);
            if (seq < m_BlockToSlot.size()) {
                uint32_t slot = m_BlockToSlot[seq];
                if (slot != UINT32_MAX && slot < m_Capacity) {
                    ReaderSpinLock guard(m_CacheSlots[slot].CS_lock);
                    if (m_CacheSlots[slot].CS_Valid && m_CacheSlots[slot].CS_BlockNum == seq) {
                        m_CacheSlots[slot].CS_Touched = true;
                        *address = &m_CacheSlots[slot].CS_Data;
                        return true;
                    }

                    *address = &m_CacheSlots[slot].CS_Data;
                    m_PendingSlot = slot;
                    return false;
                }
            }

            uint32_t victim = PickVictim();
            { WriterSpinLock guard(m_CacheSlots[victim].CS_lock);
              uint32_t oldBlock = m_CacheSlots[victim].CS_BlockNum;
              if (oldBlock < m_BlockToSlot.size() && m_BlockToSlot[oldBlock] == victim)
                  m_BlockToSlot[oldBlock] = UINT32_MAX;
              m_CacheSlots[victim].CS_BlockNum = seq;
              m_CacheSlots[victim].CS_Valid    = false;
              m_CacheSlots[victim].CS_Touched  = false; }
            EnsureBlockMapSize(seq + 1);
            m_BlockToSlot[seq] = victim;
            *address      = &m_CacheSlots[victim].CS_Data;
            m_PendingSlot = victim;
            return false;
        }

        void Add(IndexBlock* block, uint32_t block_seq) {
            if (!block) return;
            if (block_seq >= m_BlockToSlot.size() || m_BlockToSlot[block_seq] == UINT32_MAX)
                return;

            uint32_t slot = m_BlockToSlot[block_seq];
            if (slot >= m_Capacity) return;
            WriterSpinLock guard(m_CacheSlots[slot].CS_lock);
            if (m_CacheSlots[slot].CS_BlockNum == block_seq) {
                if (block != &m_CacheSlots[slot].CS_Data) m_CacheSlots[slot].CS_Data = *block;
                m_CacheSlots[slot].CS_Valid = m_CacheSlots[slot].CS_Touched = true;
            }
        }

        uint32_t capacity() const { return m_Capacity; }
        bool isPinned() const { return m_IsPinned; }
        void ReserveBlockMap(uint32_t block_count) { EnsureBlockMapSize(block_count); }

    private:
        uint32_t                     m_Capacity;
        std::unique_ptr<CacheSlot[]> m_CacheSlots;
        std::vector<uint32_t>        m_BlockToSlot;
        uint32_t                     m_Hand;
        uint32_t                     m_PendingSlot = 0;
        size_t                       m_PinBytes = 0;
        bool                         m_IsPinned = false;

        void EnsureBlockMapSize(uint32_t size) {
            if (m_BlockToSlot.size() < size)
                m_BlockToSlot.resize(size, UINT32_MAX);
        }

        uint32_t PickVictim() {
            for (uint32_t i = 0; i < m_Capacity * 2; ++i) {
                uint32_t c = m_Hand; m_Hand = (m_Hand + 1) % m_Capacity;
                if (!m_CacheSlots[c].CS_Valid) return c;
                if (!m_CacheSlots[c].CS_Touched) { m_CacheSlots[c].CS_Valid = false; return c; }
                m_CacheSlots[c].CS_Touched = false;
            }
            uint32_t v = m_Hand; m_Hand = (m_Hand + 1) % m_Capacity; return v;
        }
};


/*
* Term lookup structure.
*
* Level 1  TermDirectoryEntry — sparse directory, always memory resident.
*   Sorted by the first term in each TermHeaderBlock.
*   Binary search maps a term to the only candidate TermHeaderBlock.
*
* Level 2  TermHeaderBlock — fixed-size group of TermHeader records.
*   Each TermHeader describes where a term's compressed posting bytes live.
*   It never stores posting bytes directly.
*
* Posting blocks are separate raw byte pages managed by GetIndexBlock().
*/

static constexpr uint32_t TERM_HEADERS_PER_BLOCK = 32;

struct TermHeader {
    std::string term;
    uint32_t    doc_freq                 = 0;
    uint32_t    posting_block_id         = 0;
    uint32_t    posting_offset           = 0;
    uint32_t    posting_length           = 0;
    uint32_t    skip_list_offset         = 0;
    uint32_t    continuation_block_count = 0;
    uint32_t    flags                    = 0;
};

struct TermHeaderBlock {
    std::vector<TermHeader> headers;
};

struct TermDirectoryEntry {
    std::string first_term;
    uint32_t    term_header_block_id = 0;
};


/* ── Continuation block marker ──────────────────────────────────────────────*/
static constexpr uint16_t BLOCK_CONTINUATION_MARKER = 0xFFFFu;

/* ── IndexBlockTable ─────────────────────────────────────────────────────────
 * Posting block manager + two-level term header table.
 *
 * Lookup path:
 *   BloomFilter.CanTermExist()                  → reject obviously absent terms
 *   Level-1 binary search on m_TermDirectory    → term_header_block_id
 *   Level-2 binary search in TermHeaderBlock    → TermHeader
 *   GetIndexBlock(header.posting_block_id)      → load posting block
 *   Decoder opens at IB_Data + header.posting_offset
 */
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

        /* Called by IndexContext::Build() and IndexContext::LoadIndex(). */
        void SetTermHeaderTable(std::vector<TermDirectoryEntry> dir,
                                std::vector<TermHeaderBlock>    blocks)
        {
            m_TermDirectory    = std::move(dir);
            m_TermHeaderBlocks = std::move(blocks);
        }

        void SetPageSkipData(std::vector<uint64_t> data) { m_PageSkipData = std::move(data); }

        const uint64_t* GetPageSkipPtr(uint32_t offset) const
        {
            if (offset == 0 || offset >= m_PageSkipData.size()) return nullptr;
            return m_PageSkipData.data() + offset;
        }

        /*
        * FindTermData — two-level lookup through TermDirectory + TermHeaderBlock.
        *
        * Step 1 — BloomFilter check (placeholder, always passes).
        * Step 2 — Level-1: binary search m_TermDirectory for the header block whose
        *           first_term <= term.
        * Step 3 — Level-2: binary search within that TermHeaderBlock for exact term.
        * Step 4 — Fill out-params from the TermHeader and return true.
        */
        bool FindTermData(const char* term,
                          uint32_t*   posting_block_id_out,
                          uint32_t*   offset_out,
                          uint32_t*   posting_length_out,
                          uint32_t*   doc_freq_out,
                          uint32_t*   continuation_block_count_out = nullptr,
                          uint32_t*   skip_list_offset_out         = nullptr) const
        {
            /* Step 1: BloomFilter */
            if (!m_BloomFilter.CanTermExist(term, std::strlen(term))) return false;
            if (m_TermDirectory.empty()) return false;

            /* Step 2: Level-1 — find TermHeaderBlock whose first_term <= term */
            auto it = std::upper_bound(m_TermDirectory.begin(), m_TermDirectory.end(), term,
                [](const char* t, const TermDirectoryEntry& e) { return t < e.first_term; });
            if (it == m_TermDirectory.begin()) return false;
            --it;
            uint32_t block_idx = it->term_header_block_id;
            if (block_idx >= m_TermHeaderBlocks.size()) return false;

            /* Step 3: Level-2 — binary search within the TermHeaderBlock */
            const auto& blk = m_TermHeaderBlocks[block_idx].headers;
            auto it2 = std::lower_bound(blk.begin(), blk.end(), term,
                [](const TermHeader& e, const char* t) { return e.term < t; });
            if (it2 == blk.end() || it2->term != term) return false;

            /* Step 4: fill out-params */
            *posting_block_id_out = it2->posting_block_id;
            *offset_out           = it2->posting_offset;
            *posting_length_out   = it2->posting_length;
            *doc_freq_out         = it2->doc_freq;

            if (continuation_block_count_out) *continuation_block_count_out = it2->continuation_block_count;
            if (skip_list_offset_out)         *skip_list_offset_out         = it2->skip_list_offset;
            return true;
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
        void ReserveBlockMap(uint32_t block_count) { m_BlockCache.ReserveBlockMap(block_count); }
        void Reset(uint32_t cache_capacity = 512)
        {
            m_FileManager.reset();
            m_BlockCache.resize(cache_capacity);
            m_TermDirectory.clear();
            m_TermHeaderBlocks.clear();
            m_PageSkipData.clear();
        }

        const std::vector<TermDirectoryEntry>& GetTermDirectory()    const { return m_TermDirectory; }
        const std::vector<TermHeaderBlock>&    GetTermHeaderBlocks() const { return m_TermHeaderBlocks; }
        const std::vector<uint64_t>&           GetPageSkipData()     const { return m_PageSkipData; }

    private:
        std::shared_ptr<FileBlockManager>        m_FileManager;
        std::shared_ptr<ElementFilter>           m_ElementFilter;
        BlockCache                               m_BlockCache;
        BloomFilter                              m_BloomFilter;

        /* Level-1: directory — (first_term_of_header_block → term_header_block_id), sorted by first_term */
        std::vector<TermDirectoryEntry>          m_TermDirectory;

        /* Level-2: fixed-size term header blocks */
        std::vector<TermHeaderBlock>             m_TermHeaderBlocks;

        std::vector<uint64_t>                    m_PageSkipData;
};

inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;
    return instance;
}

constexpr uint64_t MAX_DOCID       = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE  = PAGE_SIZE;

#endif
