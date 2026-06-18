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
static constexpr size_t LEAF_TERM_ENTRY_MIN_BYTES = sizeof(uint16_t) + 6 * sizeof(uint32_t);
static constexpr size_t LEAF_TERM_ENTRY_MAX = (PAGE_SIZE - sizeof(uint32_t)) / LEAF_TERM_ENTRY_MIN_BYTES;
static constexpr uint8_t  INDEX_FILE_MAGIC[8] = {'M','O','O','N','S','H','O','T'};
static constexpr uint32_t INDEX_FORMAT_VERSION = 11;

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
static_assert(sizeof(IndexFileHeader) == 88, "IndexFileHeader must be exactly 88 bytes");
static_assert(offsetof(IndexFileHeader, IFH_LeafTermBlockOffset) == 48, "LeafTermBlock offset field must remain at byte 48");
static_assert(offsetof(IndexFileHeader, IFH_LeafTermBlockCount) == 56, "LeafTermBlock count field must remain at byte 56");

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
        assert(term.size() <= HEAD_TERM_KEY_MAX);
        if (term.size() > HEAD_TERM_KEY_MAX) {
            HTE_FirstTermLength = 0;
            std::memset(HTE_FirstTerm, 0, sizeof(HTE_FirstTerm));
            return;
        }
        HTE_FirstTermLength = static_cast<uint16_t>(term.size());
        std::memset(HTE_FirstTerm, 0, sizeof(HTE_FirstTerm));
        if (!term.empty())
            std::memcpy(HTE_FirstTerm, term.data(), term.size());
    }
};
static_assert(sizeof(HeadTermEntry) == 32, "HeadTermEntry must be fixed 32 bytes");
static_assert(alignof(HeadTermEntry) == 16, "HeadTermEntry must be 16-byte aligned");

struct LeafTermBlock {
    uint8_t LTB_Data[PAGE_SIZE] = {};
};
static_assert(sizeof(LeafTermBlock) == PAGE_SIZE, "LeafTermBlock must be exactly PAGE_SIZE bytes");

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
 *   Level-2 binary search in decoded leaf block entries → LeafTermEntry
 *   GetIndexBlock(entry.LTE_IndexBlockID)       → load posting block
 *   Decoder opens at IB_Data + entry.LTE_IndexOffset
 */
class IndexBlockTable
{
    public:
        enum class BlockKind : uint8_t {
            Index,
            LeafTerm
        };

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

        void InsertBlock(uint32_t block_seq, const IndexBlock* block)
        {
            if (!block) return;
            void* addr = GetBlock(BlockKind::Index, block_seq);
            if (!addr) return;
            std::memcpy(addr, block, sizeof(IndexBlock));
        }

        void SetHeadTermEntries(std::unique_ptr<HeadTermEntry[]> head, uint32_t headCount)
        {
            m_HeadTermEntries = std::move(head);
            m_HeadTermEntryCount = headCount;
        }

        void SetHeadTermEntries(const HeadTermEntry* head, uint32_t headCount)
        {
            std::unique_ptr<HeadTermEntry[]> entries;
            if (headCount > 0) {
                entries.reset(new HeadTermEntry[headCount]);
                std::memcpy(entries.get(), head, static_cast<size_t>(headCount) * sizeof(HeadTermEntry));
            }
            SetHeadTermEntries(std::move(entries), headCount);
        }

        /*
        * FindTermData — two-level lookup through HeadTermEntry + leaf term block.
        *
        * Step 1 — BloomFilter check (placeholder, always passes).
        * Step 2 — Level-1: binary search m_HeadTermEntries for the leaf block whose
        *           HTE_FirstTerm <= term.
        * Step 3 — Level-2: binary search within decoded leaf block entries for exact term.
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
            if (!m_HeadTermEntries || m_HeadTermEntryCount == 0) return false;
            if (std::strlen(term) > HEAD_TERM_KEY_MAX) return false;

            /* Step 2: Level-1 — find leaf block whose first term <= term */
            const std::string_view termText(term);
            const HeadTermEntry* begin = m_HeadTermEntries.get();
            const HeadTermEntry* end = begin + m_HeadTermEntryCount;
            auto it = std::upper_bound(begin, end, termText,
                [](std::string_view t, const HeadTermEntry& e) { return t < e.FirstTerm(); });
            if (it == begin) return false;
            --it;

            uint32_t blockID = it->HTE_LeafTermBlockID;
            if (blockID >= m_LeafTermBlockCount) return false;
            LeafTermEntry entries[LEAF_TERM_ENTRY_MAX];
            uint32_t decodedEntryCount = 0;
            if (!LoadLeafTermBlock(blockID, entries, LEAF_TERM_ENTRY_MAX, decodedEntryCount)) return false;
            const LeafTermEntry* begin2 = entries;
            const LeafTermEntry* end2 = begin2 + decodedEntryCount;
            auto it2 = std::lower_bound(begin2, end2, term,
                [](const LeafTermEntry& e, const char* t) { return e.LTE_Term < t; });
            if (it2 == end2 || it2->LTE_Term != term) return false;

            *indexBlockIDOut = it2->LTE_IndexBlockID;
            *indexOffsetOut  = it2->LTE_IndexOffset;
            *indexLengthOut  = it2->LTE_IndexLength;
            *docFreqOut      = it2->LTE_DocFreq;
            if (continuationBlockCountOut) *continuationBlockCountOut = it2->LTE_ContinuationBlockCount;
            return true;
        }

        IndexBlock* GetIndexBlock(uint32_t block_seq, uint32_t)
        {
            void* address = GetBlock(BlockKind::Index, block_seq);
            if (!address) return nullptr;
            return reinterpret_cast<IndexBlock*>(address);
        }

        LeafTermBlock* GetLeafTermBlock(uint32_t block_seq)
        {
            void* address = GetBlock(BlockKind::LeafTerm, block_seq);
            if (!address) return nullptr;
            return reinterpret_cast<LeafTermBlock*>(address);
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
            m_LeafTermBlockCount = leafTermBlockCount;
        }

    private:
        std::shared_ptr<ElementFilter>           m_ElementFilter;
        BlockCachePool                           m_IndexPool;
        BlockCachePool                           m_LeafTermPool;
        BloomFilter                              m_BloomFilter;

        /* Level-1: fixed directory — (HTE_FirstTerm → HTE_LeafTermBlockID), sorted by HTE_FirstTerm */
        std::unique_ptr<HeadTermEntry[]>         m_HeadTermEntries;
        uint32_t                                 m_HeadTermEntryCount = 0;

        uint32_t                                 m_LeafTermBlockCount = 0;

        static bool DecodeLeafTermBlock(const LeafTermBlock& block,
                                        LeafTermEntry* entries,
                                        uint32_t entryCapacity,
                                        uint32_t& entryCountOut)
        {
            entryCountOut = 0;
            if (!entries) return false;
            const uint8_t* ptr = block.LTB_Data;
            const uint8_t* end = block.LTB_Data + sizeof(block.LTB_Data);

            if (ptr + sizeof(uint32_t) > end) return false;
            uint32_t entryCount = 0;
            std::memcpy(&entryCount, ptr, sizeof(entryCount));
            ptr += sizeof(entryCount);
            if (entryCount > LEAF_TERM_ENTRY_MAX || entryCount > entryCapacity) return false;

            for (uint32_t i = 0; i < entryCount; ++i) {
                if (ptr + sizeof(uint16_t) > end) return false;
                uint16_t len = 0;
                std::memcpy(&len, ptr, sizeof(len));
                ptr += sizeof(len);
                if (len > HEAD_TERM_KEY_MAX) return false;
                if (ptr + len + 6 * sizeof(uint32_t) > end) return false;
                LeafTermEntry& entry = entries[i];
                entry.LTE_Term.assign(reinterpret_cast<const char*>(ptr), len);
                ptr += len;

                std::memcpy(&entry.LTE_DocFreq, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                std::memcpy(&entry.LTE_IndexBlockID, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                std::memcpy(&entry.LTE_IndexOffset, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                std::memcpy(&entry.LTE_IndexLength, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                std::memcpy(&entry.LTE_ContinuationBlockCount, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                std::memcpy(&entry.LTE_Flags, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                ++entryCountOut;
            }
            return entryCountOut == entryCount;
        }

        bool LoadLeafTermBlock(uint32_t blockID,
                               LeafTermEntry* entries,
                               uint32_t entryCapacity,
                               uint32_t& entryCountOut) const
        {
            LeafTermBlock* block = const_cast<IndexBlockTable*>(this)->GetLeafTermBlock(blockID);
            if (!block) return false;
            return DecodeLeafTermBlock(*block, entries, entryCapacity, entryCountOut);
        }

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

inline IndexBlockTable& GetIndexBlockTable()
{
    static IndexBlockTable instance;
    return instance;
}

constexpr uint64_t MAX_DOCID       = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE  = PAGE_SIZE;

#endif
