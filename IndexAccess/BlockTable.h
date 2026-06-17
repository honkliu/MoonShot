#ifndef BLOCKTABLE_H__
#define BLOCKTABLE_H__

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstddef>
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
static constexpr uint8_t  INDEX_FILE_MAGIC[8] = {'M','O','O','N','S','H','O','T'};
static constexpr uint32_t INDEX_FORMAT_VERSION = 11;

static constexpr uint64_t IB_HEADER_HAS_MORE = (1ULL << 63);
static constexpr uint16_t BLOCK_CONTINUATION_MARKER = 0xFFFFu;
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
    uint64_t IFH_HeadTermEntrySize;
    uint64_t IFH_LeafTermPageOffset;
    uint64_t IFH_LeafTermPageSize;
    uint64_t IFH_DocDataOffset;
    uint64_t IFH_DocDataSize;
    uint64_t IFH_IndexBlockOffset;
    uint64_t IFH_IndexBlockSize;
};
#pragma pack(pop)
static_assert(sizeof(IndexFileHeader) == 96, "IndexFileHeader must be exactly 96 bytes");

#pragma pack(push,1)
struct DocDataEntry {
    uint64_t DDE_DocID;
    uint64_t DDE_SourceFlags;
    uint64_t DDE_LastModifiedEpochSeconds;
    uint64_t DDE_CreatedEpochSeconds;

    uint32_t DDE_DocLength;
    uint32_t DDE_PolicyFlags;
    uint32_t DDE_VectorFlags;

    uint16_t DDE_StaticRankHalf;
    uint16_t DDE_QualityScoreHalf;
    uint16_t DDE_FreshnessScoreHalf;
    uint16_t DDE_ClickScoreHalf;
    uint16_t DDE_EngagementScoreHalf;
    uint16_t DDE_AuthorityScoreHalf;
    uint16_t DDE_SpamScoreHalf;

    uint16_t DDE_PathLength;
    uint16_t DDE_Language;
    uint16_t DDE_Locale;
    uint16_t DDE_ContentType;

    uint16_t DDE_FeatureScoreHalf[16];
    uint16_t DDE_VectorDim;
    uint16_t DDE_VectorFormat;
    uint8_t  DDE_Reserved[154];
    int8_t   DDE_VectorData[DOC_VECTOR_STORAGE_MAX_DIM];
    uint8_t  DDE_Path[DOC_PATH_MAX];
};
#pragma pack(pop)
static_assert(sizeof(DocDataEntry) == DOC_REC_SIZE, "DocDataEntry must be exactly DOC_REC_SIZE bytes");
static_assert(offsetof(DocDataEntry, DDE_Path) == DOC_REC_SIZE - DOC_PATH_MAX, "DocDataEntry path must occupy the tail of the entry");
static_assert(offsetof(DocDataEntry, DDE_VectorData) == 256, "DocDataEntry vector storage must start at byte 256");
static_assert(sizeof(DocDataEntry::DDE_VectorData) == 512, "DocDataEntry vector storage must be 512 bytes (int8[512])");
static_assert(offsetof(DocDataEntry, DDE_DocID) % 8 == 0, "DDE_DocID must be 64-bit aligned");
static_assert(offsetof(DocDataEntry, DDE_SourceFlags) % 8 == 0, "DDE_SourceFlags must be 64-bit aligned");
static_assert(offsetof(DocDataEntry, DDE_LastModifiedEpochSeconds) % 8 == 0, "DDE_LastModifiedEpochSeconds must be 64-bit aligned");
static_assert(offsetof(DocDataEntry, DDE_CreatedEpochSeconds) % 8 == 0, "DDE_CreatedEpochSeconds must be 64-bit aligned");

static inline uint16_t EncodeFloat16(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu)
        return static_cast<uint16_t>(sign | (mantissa ? 0x7e00u : 0x7c00u));

    int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
    if (halfExponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);

    if (halfExponent <= 0) {
        if (halfExponent < -10)
            return sign;

        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
        uint16_t halfMantissa = static_cast<uint16_t>(mantissa >> shift);
        if ((mantissa >> (shift - 1)) & 1u)
            ++halfMantissa;
        return static_cast<uint16_t>(sign | halfMantissa);
    }

    uint16_t half = static_cast<uint16_t>(sign | (static_cast<uint16_t>(halfExponent) << 10) | (mantissa >> 13));
    if (mantissa & 0x1000u)
        ++half;
    return half;
}

static inline float DecodeFloat16(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x03ffu;
    uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int32_t normalizedExponent = -14;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --normalizedExponent;
            }
            mantissa &= 0x03ffu;
            bits = sign
                 | (static_cast<uint32_t>(normalizedExponent + 127) << 23)
                 | (mantissa << 13);
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

static inline uint16_t EncodeDocPath(std::string_view path, uint8_t* output)
{
    if (!output || path.empty())
        return 0;

    const size_t rawBytes = std::min(path.size(), DOC_PATH_MAX);
    std::memcpy(output, path.data(), rawBytes);
    return static_cast<uint16_t>(rawBytes);
}

static inline std::string DecodeDocPath(const DocDataEntry& entry)
{
    const size_t byteCount = entry.DDE_PathLength;
    if (byteCount == 0 || byteCount > DOC_PATH_MAX)
        return {};

    return std::string(
        reinterpret_cast<const char*>(entry.DDE_Path),
        reinterpret_cast<const char*>(entry.DDE_Path + byteCount));
}

struct IndexBlock {
    uint64_t IB_Header;
    uint8_t  IB_Data[PAGE_SIZE - static_cast<int>(sizeof(uint64_t))];
};
static_assert(sizeof(IndexBlock) == PAGE_SIZE, "IndexBlock must be exactly PAGE_SIZE bytes");

struct LeafTermEntry {
    std::string LTE_Term;
    uint32_t    LTE_DocFreq                 = 0;
    uint32_t    LTE_IndexBlockID            = 0;
    uint32_t    LTE_IndexOffset             = 0;
    uint32_t    LTE_IndexLength             = 0;
    uint32_t    LTE_ContinuationBlockCount  = 0;
    uint32_t    LTE_Flags                   = 0;
};

struct LeafTermBlock {
    std::vector<LeafTermEntry> LTB_Entries;
};

struct alignas(16) HeadTermEntry {
    uint32_t HTE_LeafTermBlockID = 0;
    uint16_t HTE_FirstTermLength = 0;
    char     HTE_FirstTerm[HEAD_TERM_KEY_MAX] = {};

    std::string_view FirstTerm() const
    {
        return std::string_view(HTE_FirstTerm, HTE_FirstTermLength);
    }

    void SetFirstTerm(std::string_view term)
    {
        const size_t bytes = std::min(term.size(), HEAD_TERM_KEY_MAX);
        HTE_FirstTermLength = static_cast<uint16_t>(bytes);
        std::memset(HTE_FirstTerm, 0, sizeof(HTE_FirstTerm));
        if (bytes > 0)
            std::memcpy(HTE_FirstTerm, term.data(), bytes);
    }
};
static_assert(sizeof(HeadTermEntry) == 32, "HeadTermEntry must be fixed 32 bytes");
static_assert(alignof(HeadTermEntry) == 16, "HeadTermEntry must be 16-byte aligned");

struct LeafTermPage {
    uint8_t LTP_Data[PAGE_SIZE] = {};
};

/*
* BloomFilter — placeholder (Tiger uses a Bloom filter to reject absent terms quickly).
* Always returns true so lookups fall through to the Head/Leaf term tables.
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

struct LeafCacheSlot {
    RWSpinLock  LCS_lock;
    uint32_t    LCS_PageID  = UINT32_MAX;
    bool        LCS_Valid   = false;
    bool        LCS_Touched = false;
    LeafTermPage LCS_Data;
};

class LeafTermBlockCache {
public:
    explicit LeafTermBlockCache(uint32_t slot_count)
        : m_Capacity(std::max<uint32_t>(slot_count, 1)),
          m_CacheSlots(new LeafCacheSlot[std::max<uint32_t>(slot_count, 1)]),
          m_Hand(0)
    {
        m_PinBytes = sizeof(LeafCacheSlot) * static_cast<size_t>(m_Capacity);
        m_IsPinned = PinMemoryPages(m_CacheSlots.get(), m_PinBytes);
    }
    ~LeafTermBlockCache() { UnpinMemoryPages(m_CacheSlots.get(), m_PinBytes); }

    void resize(uint32_t new_capacity) {
        new_capacity = std::max<uint32_t>(new_capacity, 1);
        UnpinMemoryPages(m_CacheSlots.get(), m_PinBytes);
        m_Capacity = new_capacity;
        m_CacheSlots.reset(new LeafCacheSlot[new_capacity]);
        m_PageToSlot.clear();
        m_Hand = m_PendingSlot = 0;
        m_PinBytes = sizeof(LeafCacheSlot) * static_cast<size_t>(m_Capacity);
        m_IsPinned = PinMemoryPages(m_CacheSlots.get(), m_PinBytes);
    }

    bool DataLoaded(uint32_t page_id, void** address) {
        if (page_id < m_PageToSlot.size()) {
            uint32_t slot = m_PageToSlot[page_id];
            if (slot != UINT32_MAX && slot < m_Capacity) {
                ReaderSpinLock guard(m_CacheSlots[slot].LCS_lock);
                if (m_CacheSlots[slot].LCS_Valid && m_CacheSlots[slot].LCS_PageID == page_id) {
                    m_CacheSlots[slot].LCS_Touched = true;
                    *address = &m_CacheSlots[slot].LCS_Data;
                    return true;
                }
                *address = &m_CacheSlots[slot].LCS_Data;
                m_PendingSlot = slot;
                return false;
            }
        }

        uint32_t victim = PickVictim();
        { WriterSpinLock guard(m_CacheSlots[victim].LCS_lock);
          uint32_t oldPage = m_CacheSlots[victim].LCS_PageID;
          if (oldPage < m_PageToSlot.size() && m_PageToSlot[oldPage] == victim)
              m_PageToSlot[oldPage] = UINT32_MAX;
          m_CacheSlots[victim].LCS_PageID = page_id;
          m_CacheSlots[victim].LCS_Valid = false;
          m_CacheSlots[victim].LCS_Touched = false; }
        EnsurePageMapSize(page_id + 1);
        m_PageToSlot[page_id] = victim;
        *address = &m_CacheSlots[victim].LCS_Data;
        m_PendingSlot = victim;
        return false;
    }

    void Add(LeafTermPage* page, uint32_t page_id) {
        if (!page || page_id >= m_PageToSlot.size()) return;
        uint32_t slot = m_PageToSlot[page_id];
        if (slot == UINT32_MAX || slot >= m_Capacity) return;
        WriterSpinLock guard(m_CacheSlots[slot].LCS_lock);
        if (m_CacheSlots[slot].LCS_PageID == page_id) {
            if (page != &m_CacheSlots[slot].LCS_Data) m_CacheSlots[slot].LCS_Data = *page;
            m_CacheSlots[slot].LCS_Valid = true;
            m_CacheSlots[slot].LCS_Touched = true;
        }
    }

    void ReservePageMap(uint32_t page_count) { EnsurePageMapSize(page_count); }

private:
    uint32_t                         m_Capacity;
    std::unique_ptr<LeafCacheSlot[]> m_CacheSlots;
    std::vector<uint32_t>            m_PageToSlot;
    uint32_t                         m_Hand;
    uint32_t                         m_PendingSlot = 0;
    size_t                           m_PinBytes = 0;
    bool                             m_IsPinned = false;

    void EnsurePageMapSize(uint32_t size) {
        if (m_PageToSlot.size() < size) m_PageToSlot.resize(size, UINT32_MAX);
    }

    uint32_t PickVictim() {
        for (uint32_t i = 0; i < m_Capacity * 2; ++i) {
            uint32_t c = m_Hand; m_Hand = (m_Hand + 1) % m_Capacity;
            if (!m_CacheSlots[c].LCS_Valid) return c;
            if (!m_CacheSlots[c].LCS_Touched) { m_CacheSlots[c].LCS_Valid = false; return c; }
            m_CacheSlots[c].LCS_Touched = false;
        }
        uint32_t v = m_Hand; m_Hand = (m_Hand + 1) % m_Capacity; return v;
    }
};
/* ── IndexBlockTable ─────────────────────────────────────────────────────────
 * Posting block manager + two-level head/leaf term table.
 *
 * Lookup path:
 *   BloomFilter.CanTermExist()                  → reject obviously absent terms
 *   Level-1 binary search on m_HeadTermEntries  → LeafTermBlockID
 *   Level-2 binary search in LeafTermBlock      → LeafTermEntry
 *   GetIndexBlock(entry.LTE_IndexBlockID)       → load posting block
 *   Decoder opens at IB_Data + entry.LTE_IndexOffset
 */
class IndexBlockTable
{
    public:
        explicit IndexBlockTable(uint32_t cache_capacity = 512)
            : m_IndexBlockCache(cache_capacity),
              m_LeafTermBlockCache(static_cast<uint32_t>(std::max<uint64_t>(LEAF_TERM_CACHE_BYTES / sizeof(LeafCacheSlot), 1)))
        {
            m_FileManager.reset();
        }

        void InsertBlock(uint32_t block_seq, const IndexBlock* block)
        {
            void* addr = nullptr;
            m_IndexBlockCache.DataLoaded(static_cast<int>(block_seq), &addr);
            if (!addr) return;
            std::memcpy(addr, block, sizeof(IndexBlock));
            m_IndexBlockCache.Add(static_cast<IndexBlock*>(addr), block_seq);
        }

        /* Called by IndexContext::Build() and IndexContext::LoadIndex(). */
        void SetDecodedHeadLeafTermTable(std::vector<HeadTermEntry> dir,
                         std::vector<LeafTermBlock> blocks)
        {
            m_HeadTermEntries = std::move(dir);
            SetDecodedLeafTermBlocks(std::move(blocks));
        }

        void SetPagedLeafTermBlocks(std::vector<HeadTermEntry> head,
                                    std::vector<LeafTermPage> pages)
        {
            m_HeadTermEntries = std::move(head);
            m_LeafTermPageCount = static_cast<uint32_t>(pages.size());
            m_LeafTermBlockCache.ReservePageMap(m_LeafTermPageCount);
            for (uint32_t i = 0; i < m_LeafTermPageCount; ++i) {
                void* addr = nullptr;
                m_LeafTermBlockCache.DataLoaded(i, &addr);
                if (!addr) continue;
                std::memcpy(addr, &pages[i], sizeof(LeafTermPage));
                m_LeafTermBlockCache.Add(static_cast<LeafTermPage*>(addr), i);
            }
        }

        void SetHeadTermEntries(std::vector<HeadTermEntry> head, uint32_t leafPageCount)
        {
            m_HeadTermEntries = std::move(head);
            m_LeafTermPageCount = leafPageCount;
            m_LeafTermBlockCache.ReservePageMap(leafPageCount);
        }

        /*
        * FindTermData — two-level lookup through HeadTermEntry + LeafTermBlock.
        *
        * Step 1 — BloomFilter check (placeholder, always passes).
        * Step 2 — Level-1: binary search m_HeadTermEntries for the leaf block whose
        *           HTE_FirstTerm <= term.
        * Step 3 — Level-2: binary search within that LeafTermBlock for exact term.
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
            if (!m_BloomFilter.CanTermExist(term, std::strlen(term))) return false;
            if (m_HeadTermEntries.empty()) return false;

            /* Step 2: Level-1 — find LeafTermBlock whose first term <= term */
            const std::string_view termText(term);
            auto it = std::upper_bound(m_HeadTermEntries.begin(), m_HeadTermEntries.end(), termText,
                [](std::string_view t, const HeadTermEntry& e) { return t < e.FirstTerm(); });
            if (it == m_HeadTermEntries.begin()) return false;
            --it;
            const auto loadPage = [&](uint32_t block_idx, LeafTermBlock& decoded) -> bool {
                if (block_idx >= m_LeafTermPageCount) return false;
                return LoadLeafTermBlock(block_idx, decoded);
            };

            const auto findInDecodedPage = [&](const LeafTermBlock& decoded) -> const LeafTermEntry* {
                const auto& blk = decoded.LTB_Entries;
                auto it2 = std::lower_bound(blk.begin(), blk.end(), term,
                    [](const LeafTermEntry& e, const char* t) { return e.LTE_Term < t; });
                if (it2 == blk.end() || it2->LTE_Term != term) return nullptr;
                thread_local LeafTermEntry found;
                found = *it2;
                return &found;
            };

            const auto pageMayContain = [&](const LeafTermBlock& decoded) -> bool {
                if (decoded.LTB_Entries.empty()) return false;
                return decoded.LTB_Entries.front().LTE_Term <= term && term <= decoded.LTB_Entries.back().LTE_Term;
            };

            const int32_t startPage = static_cast<int32_t>(it->HTE_LeafTermBlockID);

            LeafTermBlock decoded;
            if (loadPage(static_cast<uint32_t>(startPage), decoded)) {
                if (const LeafTermEntry* found = findInDecodedPage(decoded)) {
                    *indexBlockIDOut = found->LTE_IndexBlockID;
                    *indexOffsetOut  = found->LTE_IndexOffset;
                    *indexLengthOut  = found->LTE_IndexLength;
                    *docFreqOut      = found->LTE_DocFreq;
                    if (continuationBlockCountOut) *continuationBlockCountOut = found->LTE_ContinuationBlockCount;
                    return true;
                }
            }

            for (int32_t page = startPage - 1; page >= 0; --page) {
                LeafTermBlock leftDecoded;
                if (!loadPage(static_cast<uint32_t>(page), leftDecoded) || leftDecoded.LTB_Entries.empty()) break;
                if (term > leftDecoded.LTB_Entries.back().LTE_Term) break;
                if (pageMayContain(leftDecoded)) {
                    if (const LeafTermEntry* found = findInDecodedPage(leftDecoded)) {
                        *indexBlockIDOut = found->LTE_IndexBlockID;
                        *indexOffsetOut  = found->LTE_IndexOffset;
                        *indexLengthOut  = found->LTE_IndexLength;
                        *docFreqOut      = found->LTE_DocFreq;
                        if (continuationBlockCountOut) *continuationBlockCountOut = found->LTE_ContinuationBlockCount;
                        return true;
                    }
                }
            }

            for (uint32_t page = static_cast<uint32_t>(startPage + 1); page < m_LeafTermPageCount; ++page) {
                LeafTermBlock rightDecoded;
                if (!loadPage(page, rightDecoded) || rightDecoded.LTB_Entries.empty()) break;
                if (term < rightDecoded.LTB_Entries.front().LTE_Term) break;
                if (pageMayContain(rightDecoded)) {
                    if (const LeafTermEntry* found = findInDecodedPage(rightDecoded)) {
                        *indexBlockIDOut = found->LTE_IndexBlockID;
                        *indexOffsetOut  = found->LTE_IndexOffset;
                        *indexLengthOut  = found->LTE_IndexLength;
                        *docFreqOut      = found->LTE_DocFreq;
                        if (continuationBlockCountOut) *continuationBlockCountOut = found->LTE_ContinuationBlockCount;
                        return true;
                    }
                }
            }

            return false;
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t)
        {
            void* address = nullptr;
            bool inCache = m_IndexBlockCache.DataLoaded(static_cast<int>(block_seq), &address);
            if (inCache) return static_cast<IndexBlock*>(address);
            if (m_FileManager && address) {
                m_FileManager->read(block_seq, address);
                m_IndexBlockCache.Add(static_cast<IndexBlock*>(address), block_seq);
                return static_cast<IndexBlock*>(address);
            }
            return nullptr;
        }

        void SetFileManager(std::shared_ptr<FileBlockManager> fm) { m_FileManager = std::move(fm); }
        void SetLeafTermFileManager(std::shared_ptr<FileBlockManager> fm) { m_LeafTermFileManager = std::move(fm); }
        void ResizeCache(uint32_t capacity) { m_IndexBlockCache.resize(capacity); }
        void ResizeLeafTermCache(uint32_t capacity) { m_LeafTermBlockCache.resize(capacity); }
        void ReserveBlockMap(uint32_t block_count) { m_IndexBlockCache.ReserveBlockMap(block_count); }
        void ReserveLeafTermPageMap(uint32_t page_count) { m_LeafTermBlockCache.ReservePageMap(page_count); }
        void Reset(uint32_t cache_capacity = 512)
        {
            m_FileManager.reset();
            m_LeafTermFileManager.reset();
            m_IndexBlockCache.resize(cache_capacity);
            m_LeafTermBlockCache.resize(static_cast<uint32_t>(std::max<uint64_t>(LEAF_TERM_CACHE_BYTES / sizeof(LeafCacheSlot), 1)));
            m_HeadTermEntries.clear();
            m_LeafTermPageCount = 0;
        }

        const std::vector<HeadTermEntry>&  GetHeadTermEntries() const { return m_HeadTermEntries; }

    private:
        std::shared_ptr<FileBlockManager>        m_FileManager;
        std::shared_ptr<FileBlockManager>        m_LeafTermFileManager;
        std::shared_ptr<ElementFilter>           m_ElementFilter;
        BlockCache                               m_IndexBlockCache;
        LeafTermBlockCache                       m_LeafTermBlockCache;
        BloomFilter                              m_BloomFilter;

        /* Level-1: fixed directory — (HTE_FirstTerm → HTE_LeafTermBlockID), sorted by HTE_FirstTerm */
        std::vector<HeadTermEntry>               m_HeadTermEntries;

        uint32_t                                 m_LeafTermPageCount = 0;

        static bool DecodeLeafTermPage(const LeafTermPage& page, LeafTermBlock& out)
        {
            out.LTB_Entries.clear();
            const uint8_t* ptr = page.LTP_Data;
            const uint8_t* end = page.LTP_Data + sizeof(page.LTP_Data);
            auto read_u16 = [&]() -> uint16_t {
                if (ptr + 2 > end) return 0;
                uint16_t v; std::memcpy(&v, ptr, 2); ptr += 2; return v;
            };
            auto read_u32 = [&]() -> uint32_t {
                if (ptr + 4 > end) return 0;
                uint32_t v; std::memcpy(&v, ptr, 4); ptr += 4; return v;
            };

            uint32_t entryCount = read_u32();
            out.LTB_Entries.reserve(entryCount);
            for (uint32_t i = 0; i < entryCount && ptr < end; ++i) {
                uint16_t len = read_u16();
                if (ptr + len + 24 > end) return false;
                LeafTermEntry entry;
                entry.LTE_Term.assign(reinterpret_cast<const char*>(ptr), len);
                ptr += len;
                entry.LTE_DocFreq = read_u32();
                entry.LTE_IndexBlockID = read_u32();
                entry.LTE_IndexOffset = read_u32();
                entry.LTE_IndexLength = read_u32();
                entry.LTE_ContinuationBlockCount = read_u32();
                entry.LTE_Flags = read_u32();
                out.LTB_Entries.push_back(std::move(entry));
            }
            return true;
        }

        bool LoadLeafTermBlock(uint32_t pageID, LeafTermBlock& out) const
        {
            void* address = nullptr;
            bool inCache = const_cast<LeafTermBlockCache&>(m_LeafTermBlockCache).DataLoaded(pageID, &address);
            if (!inCache && m_LeafTermFileManager && address) {
                m_LeafTermFileManager->read(pageID, address);
                const_cast<LeafTermBlockCache&>(m_LeafTermBlockCache).Add(static_cast<LeafTermPage*>(address), pageID);
            } else if (!inCache) {
                return false;
            }
            return DecodeLeafTermPage(*static_cast<LeafTermPage*>(address), out);
        }

        void SetDecodedLeafTermBlocks(std::vector<LeafTermBlock> blocks)
        {
            std::vector<LeafTermPage> pages;
            pages.reserve(blocks.size());
            for (const auto& block : blocks) {
                LeafTermPage page{};
                uint8_t* ptr = page.LTP_Data;
                auto write_u16 = [&](uint16_t v) { std::memcpy(ptr, &v, 2); ptr += 2; };
                auto write_u32 = [&](uint32_t v) { std::memcpy(ptr, &v, 4); ptr += 4; };
                write_u32(static_cast<uint32_t>(block.LTB_Entries.size()));
                for (const auto& e : block.LTB_Entries) {
                    write_u16(static_cast<uint16_t>(e.LTE_Term.size()));
                    std::memcpy(ptr, e.LTE_Term.data(), e.LTE_Term.size()); ptr += e.LTE_Term.size();
                    write_u32(e.LTE_DocFreq);
                    write_u32(e.LTE_IndexBlockID);
                    write_u32(e.LTE_IndexOffset);
                    write_u32(e.LTE_IndexLength);
                    write_u32(e.LTE_ContinuationBlockCount);
                    write_u32(e.LTE_Flags);
                }
                pages.push_back(page);
            }
            SetPagedLeafTermBlocks(std::move(m_HeadTermEntries), std::move(pages));
        }
};

inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;
    return instance;
}

constexpr uint64_t MAX_DOCID       = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE  = PAGE_SIZE;

#endif
