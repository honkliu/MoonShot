#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
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
static constexpr size_t DOC_REC_SIZE = 1024;
static constexpr size_t DOC_VECTOR_DIM = 128;
static constexpr size_t DOC_VECTOR_STORAGE_MAX_DIM = 512;  // int8[512]
static constexpr size_t DOC_PATH_MAX = 256;
static constexpr size_t HEAD_TERM_KEY_MAX = 26;
static constexpr size_t LEAF_TERM_ENTRY_MIN_BYTES = 6 * sizeof(uint32_t) + sizeof(uint16_t);
static constexpr size_t LEAF_TERM_ENTRY_MAX = (PAGE_SIZE - sizeof(uint32_t)) / LEAF_TERM_ENTRY_MIN_BYTES;
static constexpr uint8_t  INDEX_FILE_MAGIC[8] = {'M','O','O','N','S','H','O','T'};
static constexpr uint32_t INDEX_FORMAT_VERSION = 12;

static constexpr uint64_t IB_HEADER_HAS_MORE = (1ULL << 63);
static constexpr uint16_t BLOCK_CONTINUATION_MARKER = 0xFFFFu;
static constexpr uint64_t INDEX_BLOCK_CACHE_BYTES = 100ull * 1024ull * 1024ull;
static constexpr uint64_t LEAF_TERM_CACHE_BYTES = 100ull * 1024ull * 1024ull;

/*
* Physical index file structures.
*
* Keep these together. They define the bytes on disk or the decoded view of
* those bytes: file header, DocData records, posting pages, and the two-level
* Head/Leaf term directory.
*/
#pragma pack(push,1)
struct IndexFileHeader {
    uint8_t  IFH_Magic[8];
    uint32_t IFH_Version;
    float    IFH_AvgDocLength;
    uint64_t IFH_NumDocuments;
    uint64_t IFH_NumTerms;
    uint64_t IFH_HeadTermEntryOffset;
    uint64_t IFH_HeadTermEntryCount;
    uint64_t IFH_LeafTermBlockOffset;
    uint64_t IFH_LeafTermBlockCount;
    uint64_t IFH_DocDataOffset;
    uint64_t IFH_IndexBlockOffset;
    uint64_t IFH_IndexBlockCount;
};
#pragma pack(pop)

#pragma pack(push,1)
struct DocDataEntry {
    uint64_t DDE_DocID;
    uint64_t DDE_SourceFlags;
    uint64_t DDE_LastModifiedEpochSeconds;
    uint64_t DDE_CreatedEpochSeconds;

    uint32_t DDE_DocLength;
    uint32_t DDE_PolicyFlags;
    uint32_t DDE_VectorFlags;

    float    DDE_StaticRank;
    float    DDE_QualityScore;
    float    DDE_FreshnessScore;
    float    DDE_ClickScore;
    float    DDE_EngagementScore;
    float    DDE_AuthorityScore;
    float    DDE_SpamScore;

    uint16_t DDE_PathLength;
    uint16_t DDE_Language;
    uint16_t DDE_Locale;
    uint16_t DDE_ContentType;

    float    DDE_FeatureScore[16];
    uint16_t DDE_VectorDim;
    uint16_t DDE_VectorFormat;
    uint8_t  DDE_Reserved[108];
    int8_t   DDE_VectorData[DOC_VECTOR_STORAGE_MAX_DIM];
    uint8_t  DDE_Path[DOC_PATH_MAX];
};
#pragma pack(pop)

struct IndexBlock {
    uint64_t IB_Header;
    uint8_t  IB_Data[PAGE_SIZE - static_cast<int>(sizeof(uint64_t))];
};

#pragma pack(push,1)
struct LeafTermEntry {
    uint32_t    LTE_DocFreq                 = 0;
    uint32_t    LTE_IndexBlockID            = 0;
    uint32_t    LTE_IndexOffset             = 0;
    uint32_t    LTE_IndexLength             = 0;
    uint32_t    LTE_ContinuationBlockCount  = 0;
    uint32_t    LTE_Flags                   = 0;
    uint16_t    LTE_TermLength              = 0;
    char        LTE_Term[0];
};
#pragma pack(pop)

struct alignas(16) HeadTermEntry {
    uint32_t HTE_LeafTermBlockID = 0;
    uint16_t HTE_FirstTermLength = 0;
    char     HTE_FirstTerm[HEAD_TERM_KEY_MAX] = {};
};

struct LeafTermBlock {
    uint8_t LTB_Data[PAGE_SIZE] = {};
};

/*
* BloomFilter — placeholder (Tiger uses a Bloom filter to reject absent terms quickly).
* Always returns true so lookups fall through to the Head/Leaf term tables.
*/
struct BloomFilter {
    bool CanTermExist(const char* /*term*/, size_t /*len*/) const { return true; }
};

enum class BlockKind {
    Index,
    LeafTerm
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

static inline bool PinMemoryPages(void* address, size_t bytes)
{
    if (!address || bytes == 0) return false;
#ifdef _WIN32
    static std::atomic<size_t> requestedLockedBytes {0};
    SIZE_T minWorkingSet = 0, maxWorkingSet = 0;
    HANDLE process = GetCurrentProcess();
    if (GetProcessWorkingSetSize(process, &minWorkingSet, &maxWorkingSet)) {
        const size_t totalLockedBytes = requestedLockedBytes.fetch_add(bytes) + bytes;
        const SIZE_T desiredMin = std::max<SIZE_T>(minWorkingSet, totalLockedBytes + (16ull * 1024ull * 1024ull));
        const SIZE_T desiredMax = std::max<SIZE_T>(maxWorkingSet, desiredMin + (16ull * 1024ull * 1024ull));
        if (minWorkingSet < desiredMin || maxWorkingSet < desiredMax)
            SetProcessWorkingSetSize(process, desiredMin, desiredMax);
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

/* ── IndexBlockTable ─────────────────────────────────────────────────────────
 * Posting block manager + two-level head/leaf term table.
 *
 * Lookup path:
 *   BloomFilter.CanTermExist()                  → reject obviously absent terms
 *   Level-1 binary search on m_HeadTermEntries  → LeafTermBlockID
 *   Level-2 scan in encoded leaf block entries → LeafTermEntry
 *   GetBlock(Index, entry.LTE_IndexBlockID)     → posting block
 *   Decoder opens at IB_Data + entry.LTE_IndexOffset
 */
class IndexBlockTable
{
    private:

        struct BlockCacheSlot {
            RWSpinLock BCS_Lock;
            uint32_t   BCS_LocalBlockID = UINT32_MAX;
            bool       BCS_Valid = false;
            bool       BCS_Touched = false;
        };

        struct BlockCachePool {
            void*                             BCP_Pages = nullptr;
            size_t                            BCP_PageSize = 0;
            uint32_t                          BCP_BlockCount = 0;
        };

    public:
        explicit IndexBlockTable(uint32_t = 0) = default;

        void SetHeadTermEntries(std::unique_ptr<HeadTermEntry[]> head, uint32_t headCount)
        {
            m_HeadTermEntries = std::move(head);
            m_HeadTermEntryCount = headCount;
        }

        /*
        * FindTermData — two-level lookup through HeadTermEntry + leaf term block.
        *
        * Step 1 — BloomFilter check (placeholder, always passes).
        * Step 2 — Level-1: binary search m_HeadTermEntries for the leaf block whose
        *           HTE_FirstTerm <= term.
        * Step 3 — Level-2: scan encoded entries inside the leaf block for exact term.
        * Step 4 — Fill out-params from the LeafTermEntry and return true.
        */
        bool FindTermData(const char* term,
                          uint32_t*   indexBlockIDOut,
                          uint32_t*   indexOffsetOut,
                          uint32_t*   indexLengthOut,
                          uint32_t*   docFreqOut,
                          uint32_t*   continuationBlockCountOut = nullptr) const
        {
            /* Step 1: BloomFilter */
            const size_t termLength = std::strlen(term);
            if (!m_BloomFilter.CanTermExist(term, termLength)) return false;
            if (!m_HeadTermEntries || m_HeadTermEntryCount == 0) return false;
            if (termLength > HEAD_TERM_KEY_MAX) return false;

            /* Step 2: Level-1 — find leaf block whose first term <= term */
            const std::string_view termText(term);
            const HeadTermEntry* begin = m_HeadTermEntries.get();
            const HeadTermEntry* end = begin + m_HeadTermEntryCount;
            auto it = std::upper_bound(begin, end, termText,
                [](std::string_view t, const HeadTermEntry& e) { return t < std::string_view(e.HTE_FirstTerm, e.HTE_FirstTermLength); });
            if (it == begin) return false;
            --it;

            uint32_t blockID = it->HTE_LeafTermBlockID;
            if (!m_LeafTermPool.BCP_Pages || blockID >= m_LeafTermPool.BCP_BlockCount) return false;
            const LeafTermBlock* block = static_cast<const LeafTermBlock*>(m_LeafTermPool.BCP_Pages) + blockID;
            const uint8_t* ptr = block->LTB_Data;
            const uint8_t* end = block->LTB_Data + sizeof(block->LTB_Data);

            if (ptr + sizeof(uint32_t) > end) return false;
            uint32_t entryCount = 0;
            std::memcpy(&entryCount, ptr, sizeof(entryCount));
            ptr += sizeof(entryCount);
            if (entryCount > LEAF_TERM_ENTRY_MAX) return false;

            for (uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
                if (ptr + sizeof(LeafTermEntry) > end) return false;
                const LeafTermEntry* entry = reinterpret_cast<const LeafTermEntry*>(ptr);
                if (entry->LTE_TermLength > HEAD_TERM_KEY_MAX) return false;
                ptr += sizeof(LeafTermEntry) + entry->LTE_TermLength;
                if (ptr > end) return false;

                const std::string_view entryTerm(entry->LTE_Term, entry->LTE_TermLength);

                if (entryTerm == termText) {
                    *docFreqOut = entry->LTE_DocFreq;
                    *indexBlockIDOut = entry->LTE_IndexBlockID;
                    *indexOffsetOut = entry->LTE_IndexOffset;
                    *indexLengthOut = entry->LTE_IndexLength;
                    if (continuationBlockCountOut) *continuationBlockCountOut = entry->LTE_ContinuationBlockCount;
                    return true;
                }

                if (entryTerm > termText) return false;
            }
            return false;
        }

        void* GetBlock(BlockKind kind, uint32_t block_seq)
        {
            BlockCachePool* pool = GetPool(kind);
            if (!pool || block_seq >= pool->BCP_BlockCount) return nullptr;
            return SlotAddress(*pool, block_seq);
        }

        void SetBlockMemory(IndexBlock* indexBlocks,
                            LeafTermBlock* leafTermBlocks,
                            uint32_t indexBlockCount,
                            uint32_t leafTermBlockCount)
        {
            SetPoolMemory(m_IndexPool, indexBlocks, sizeof(IndexBlock), indexBlockCount);
            SetPoolMemory(m_LeafTermPool, leafTermBlocks, sizeof(LeafTermBlock), leafTermBlockCount);
        }

    private:
        std::shared_ptr<ElementFilter>           m_ElementFilter;
        BlockCachePool                           m_IndexPool;
        BlockCachePool                           m_LeafTermPool;
        BloomFilter                              m_BloomFilter;

        /* Level-1: fixed directory — (HTE_FirstTerm → HTE_LeafTermBlockID), sorted by HTE_FirstTerm */
        std::unique_ptr<HeadTermEntry[]>         m_HeadTermEntries;
        uint32_t                                 m_HeadTermEntryCount = 0;

        BlockCachePool* GetPool(BlockKind kind)
        {
            return kind == BlockKind::Index ? &m_IndexPool : &m_LeafTermPool;
        }

        static void SetPoolMemory(BlockCachePool& pool, void* pages, size_t pageSize, uint32_t blockCount)
        {
            pool.BCP_Pages = pages;
            pool.BCP_PageSize = pageSize;
            pool.BCP_BlockCount = pages ? blockCount : 0;
        }

        static void* SlotAddress(BlockCachePool& pool, uint32_t blockID)
        {
            if (!pool.BCP_Pages || pool.BCP_PageSize == 0 || blockID >= pool.BCP_BlockCount)
                return nullptr;

            return static_cast<uint8_t*>(pool.BCP_Pages) + static_cast<size_t>(blockID) * pool.BCP_PageSize;
        }
};

constexpr uint64_t MAX_DOCID       = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE  = PAGE_SIZE;

#endif
