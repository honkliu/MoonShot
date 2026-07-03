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
#include <utility>
#include <condition_variable>
#include <deque>
#include <latch>
#include <mutex>
#include "../Utils/FileAccess.h"
#include "ElementFilter.h"
#include "MemOperation.h"

static constexpr int PAGE_SIZE  = 4096;
static constexpr size_t DOC_REC_SIZE = 256;
static constexpr size_t DOC_VECTOR_DIM = 128;
static constexpr size_t DOC_VECTOR_STORAGE_MAX_DIM = DOC_VECTOR_DIM;  // fixed int8[128]
static constexpr size_t DOC_PATH_MAX = 64;
static constexpr size_t DOC_PATH_PREFIX_ID_BYTES = 2;
static constexpr size_t DOC_PATH_FILENAME_MAX = DOC_PATH_MAX - DOC_PATH_PREFIX_ID_BYTES;
static constexpr uint16_t DOC_PATH_PREFIX_INVALID = UINT16_MAX;
static constexpr size_t HEAD_TERM_KEY_MAX = 26;
static constexpr size_t LEAF_TERM_DIRECTORY_COUNT = 161;
static constexpr size_t LEAF_TERM_DATA_OFFSET = LEAF_TERM_DIRECTORY_COUNT * sizeof(uint16_t);
static constexpr size_t PATH_PREFIX_SIDECAR_PAGE_COUNT = 10;
static constexpr size_t PATH_PREFIX_SIDECAR_BYTES = PATH_PREFIX_SIDECAR_PAGE_COUNT * PAGE_SIZE;
static constexpr uint8_t  INDEX_FILE_MAGIC[8] = {'M','O','O','N','S','H','O','T'};
static constexpr uint32_t INDEX_FORMAT_VERSION = 20;
static constexpr uint8_t  PATH_PREFIX_SIDECAR_MAGIC[8] = {'M','S','P','A','T','H','S','\0'};
static constexpr uint16_t PATH_PREFIX_SIDECAR_VERSION = 1;

static constexpr uint64_t INDEX_BLOCK_CACHE_BYTES = 100ull * 1024ull * 1024ull;
static constexpr uint64_t LEAF_TERM_CACHE_BYTES = 100ull * 1024ull * 1024ull;

inline uint64_t TermMphfHash(const char* term, size_t len, uint64_t seed)
{
    uint64_t hash = 1469598103934665603ull ^ seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint8_t>(term[i]);
        hash *= 1099511628211ull;
    }
    hash ^= hash >> 32;
    hash *= 0xd6e8feb86659fd93ull;
    hash ^= hash >> 32;
    return hash;
}

inline uint64_t TermMphfSlotSeed(uint64_t seed, uint32_t displacement)
{
    uint64_t x = seed ^ (0x9e3779b97f4a7c15ull * (static_cast<uint64_t>(displacement) + 1));
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

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
    uint64_t IFH_TermMphfHeaderOffset;
    uint64_t IFH_TermMphfHeaderCount;
    uint64_t IFH_TermMphfDisplacementOffset;
    uint64_t IFH_TermMphfDisplacementCount;
    uint64_t IFH_TermMphfEntryOffset;
    uint64_t IFH_TermMphfEntryPageCount;
};
#pragma pack(pop)

static constexpr uint64_t TERM_MPHF_MAGIC = 0x4850464d4d524554ull; // "TERMMFPH" little-endian tag

#pragma pack(push,1)
struct TermMphfHeader {
    uint64_t TMH_Magic = TERM_MPHF_MAGIC;
    uint64_t TMH_TermCount = 0;
    uint32_t TMH_BucketCount = 0;
    uint32_t TMH_SlotCount = 0;
    uint64_t TMH_BucketSeed = 0;
    uint64_t TMH_SlotSeed = 0;
    uint64_t TMH_FingerprintSeed = 0;
};
#pragma pack(pop)

#pragma pack(push,1)
struct PathPrefixSidecarHeader {
    uint8_t  PPSH_Magic[8] = {'M','S','P','A','T','H','S','\0'};
    uint16_t PPSH_Version = PATH_PREFIX_SIDECAR_VERSION;
    uint16_t PPSH_PrefixCount = 0;
    uint32_t PPSH_EntryOffset = sizeof(PathPrefixSidecarHeader);
    uint32_t PPSH_StringOffset = sizeof(PathPrefixSidecarHeader);
    uint32_t PPSH_StringBytes = 0;
    uint8_t  PPSH_Reserved[8] = {};
};

struct PathPrefixSidecarEntry {
    uint32_t PPSE_Offset = 0;
    uint16_t PPSE_Length = 0;
    uint16_t PPSE_Flags = 0;
};
#pragma pack(pop)

static_assert(sizeof(PathPrefixSidecarHeader) == 32);
static_assert(sizeof(PathPrefixSidecarEntry) == 8);

#pragma pack(push,1)
struct DocDataEntry {
    uint32_t DDE_DocID;

    uint16_t DDE_StaticRank;
    uint16_t DDE_QualityScore;
    uint16_t DDE_FreshnessScore;
    uint16_t DDE_ClickScore;
    uint16_t DDE_EngagementScore;
    uint16_t DDE_AuthorityScore;
    uint16_t DDE_SpamScore;

    uint16_t DDE_PathLength;
    uint16_t DDE_Language;
    uint16_t DDE_Locale;
    uint16_t DDE_ContentType;

    uint32_t DDE_TitleLength;
    uint32_t DDE_BodyLength;
    uint32_t DDE_UrlLength;
    uint32_t DDE_AnchorLength;
    uint32_t DDE_MetaLength;
    float    DDE_DiversityScore;
    float    DDE_LengthQualityScore;
    uint16_t DDE_VectorDim;
    uint16_t DDE_VectorFormat;
    uint8_t  DDE_Reserved[6];
    int8_t   DDE_VectorData[DOC_VECTOR_STORAGE_MAX_DIM];
    uint8_t  DDE_Path[DOC_PATH_MAX];
};
#pragma pack(pop)
static_assert(sizeof(DocDataEntry) == DOC_REC_SIZE);

inline uint16_t DocDataEncodeScore(float value)
{
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return UINT16_MAX;
    return static_cast<uint16_t>(value * 65535.0f + 0.5f);
}

inline float DocDataDecodeScore(uint16_t value)
{
    return static_cast<float>(value) / 65535.0f;
}

#pragma pack(push,1)
struct IndexBlockContinuationHeader {
    uint64_t IBCH_MaxDocID;
    uint32_t IBCH_DataLength;
};
#pragma pack(pop)

struct alignas(8) IndexBlock {
    uint8_t IB_Data[PAGE_SIZE];
};
static_assert(sizeof(IndexBlock) == PAGE_SIZE);

#pragma pack(push,1)
struct LeafTermEntry {
    uint32_t    LTE_DocFreq                 = 0;
    uint32_t    LTE_IndexBlockID            = 0;
    uint16_t    LTE_IndexOffset             = 0;
    uint16_t    LTE_IndexLength             = 0;
    uint16_t    LTE_ContinuationBlockCount  = 0;
    uint8_t     LTE_Flags                   = 0;
    uint8_t     LTE_TermLength              = 0;

    char        LTE_Term[0];
};
#pragma pack(pop)

/* Kept only for reading legacy MPHF pages; new indexes do not build MPHF. */
#pragma pack(push,1)
struct TermMphfEntry {
    uint32_t    LTE_DocFreq                 = 0;
    uint32_t    LTE_IndexBlockID            = 0;
    uint32_t    LTE_IndexOffset             = 0;
    uint32_t    LTE_IndexLength             = 0;
    uint32_t    LTE_ContinuationBlockCount  = 0;
    uint32_t    LTE_Flags                   = 0;
    uint64_t    LTE_Fingerprint             = 0;
};
#pragma pack(pop)

static_assert(sizeof(LeafTermEntry) == 16);
static_assert(sizeof(TermMphfEntry) == 32);
static_assert(PAGE_SIZE % sizeof(TermMphfEntry) == 0);

struct alignas(16) HeadTermEntry {
    uint32_t HTE_LeafTermBlockID = 0;
    uint16_t HTE_FirstTermLength = 0;
    char     HTE_FirstTerm[HEAD_TERM_KEY_MAX] = {};
};

/*
* LeafTermBlock is one 4096-byte page.
* LTB_Directory[0..159] stores LeafTermEntry offsets from the block base.
* LTB_Directory[160] stores the number of entries in this block.
* LTB_Data stores packed LeafTermEntry records followed by their term bytes.
*/
struct LeafTermBlock {
    uint16_t LTB_Directory[LEAF_TERM_DIRECTORY_COUNT] = {};
    uint8_t  LTB_Data[PAGE_SIZE - LEAF_TERM_DATA_OFFSET] = {};
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

/* ── IndexBlockTable ─────────────────────────────────────────────────────────
 * Posting block manager + two-level head/leaf term table.
 *
 * Lookup path:
 *   BloomFilter.CanTermExist()                  → reject obviously absent terms
 *   Level-1 binary search on m_HeadTermEntries  → LeafTermBlockID
 *   Level-2 binary search on LeafTermBlock offsets → LeafTermEntry
 *   GetBlock(Index, entry.LTE_IndexBlockID)     → posting block
 *   Decoder opens at IB_Data + entry.LTE_IndexOffset
 */
class IndexBlockTable
{
    public:
        struct IndexSlotEntry {
            uint32_t BlockID = UINT32_MAX;
            uint32_t Ref = 0;
        };

    private:
        enum class BlockRequestType {
            Get,
            Release
        };

        struct BlockRequest {
            BlockRequestType Type = BlockRequestType::Get;
            uint32_t BlockSeq = 0;
            uint32_t Slot = UINT32_MAX;
            void* Address = nullptr;
            std::latch Completion{1};
        };

    public:
        struct BlockCachePool {
            uint8_t*                          BCP_Pages = nullptr;
            uint64_t                          BCP_BaseOffset = 0;
            uint32_t                          BCP_TotalBlockCount = 0;
            uint32_t                          BCP_SlotCount = 0;
            uint32_t                          BCP_EvictSlot = 0;
            uint32_t*                         BCP_LogicTable = nullptr;
            IndexSlotEntry*                   BCP_SlotTable = nullptr;
            FileAccess*                       BCP_File = nullptr;
            std::thread                       BCP_Thread;
            std::mutex                        BCP_RequestMutex;
            std::condition_variable           BCP_RequestCv;
            std::deque<BlockRequest*>         BCP_Requests;
            bool                              BCP_ExitThread = false;
        };

        explicit IndexBlockTable(uint32_t = 0) {}

        ~IndexBlockTable()
        {
            ExitBlockThread(m_IndexPool);
            ExitBlockThread(m_LeafTermPool);
            if (m_IndexPool.BCP_Pages)
                PinnedMemFree(m_IndexPool.BCP_Pages);
            if (m_IndexPool.BCP_LogicTable)
                PinnedMemFree(m_IndexPool.BCP_LogicTable);
            if (m_IndexPool.BCP_SlotTable)
                PinnedMemFree(m_IndexPool.BCP_SlotTable);
            delete m_IndexPool.BCP_File;
            if (m_LeafTermPool.BCP_Pages)
                PinnedMemFree(m_LeafTermPool.BCP_Pages);
            if (m_LeafTermPool.BCP_LogicTable)
                PinnedMemFree(m_LeafTermPool.BCP_LogicTable);
            if (m_LeafTermPool.BCP_SlotTable)
                PinnedMemFree(m_LeafTermPool.BCP_SlotTable);
            delete m_LeafTermPool.BCP_File;
            ClearTermMphf();
        }

        void SetHeadTermEntries(std::unique_ptr<HeadTermEntry[]> head, uint32_t headCount)
        {
            m_HeadTermEntries = std::move(head);
            m_HeadTermEntryCount = headCount;
        }

        void SetTermMphf(const TermMphfHeader& header, std::unique_ptr<int32_t[]> displacements, uint32_t displacementCount, uint8_t* entryPages, uint32_t entryPageCount)
        {
            ClearTermMphf();
            if (header.TMH_TermCount == 0 || header.TMH_BucketCount == 0 || header.TMH_SlotCount == 0 || !displacements || !entryPages || entryPageCount == 0)
                return;
            const uint64_t requiredBytes = header.TMH_SlotCount * sizeof(TermMphfEntry);
            const uint64_t availableBytes = static_cast<uint64_t>(entryPageCount) * PAGE_SIZE;
            if (header.TMH_Magic != TERM_MPHF_MAGIC
                || header.TMH_SlotCount != header.TMH_TermCount
                || displacementCount != header.TMH_BucketCount
                || requiredBytes > availableBytes)
                return;

            m_TermMphfHeader = header;
            m_TermMphfDisplacements = std::move(displacements);
            m_TermMphfDisplacementCount = displacementCount;
            m_TermMphfEntryPages = entryPages;
            m_TermMphfEntryPageCount = entryPageCount;
        }

        void SetTermMphfEnabled(bool enabled)
        {
            m_TermMphfEnabled = enabled;
        }

        void SetDirectBlockAccessEnabled(bool enabled)
        {
            if (m_DirectBlockAccess == enabled)
                return;

            m_DirectBlockAccess = enabled;
            if (m_DirectBlockAccess) {
                ExitBlockThread(m_IndexPool);
                ExitBlockThread(m_LeafTermPool);
            } else {
                StartBlockThread(m_IndexPool);
                StartBlockThread(m_LeafTermPool);
            }
        }

        void HandOverBlockTable(IndexBlockTable& source)
        {
            if (this == &source) return;

            SetBlockMemory(nullptr, nullptr);
            m_HeadTermEntries.reset();
            m_HeadTermEntryCount = 0;
            ClearTermMphf();

            source.ExitBlockThread(source.m_IndexPool);
            source.ExitBlockThread(source.m_LeafTermPool);

            HandOverPool(m_IndexPool, source.m_IndexPool);
            HandOverPool(m_LeafTermPool, source.m_LeafTermPool);
            m_ElementFilter = std::move(source.m_ElementFilter);
            m_HeadTermEntries = std::move(source.m_HeadTermEntries);
            m_HeadTermEntryCount = source.m_HeadTermEntryCount;
            source.m_HeadTermEntryCount = 0;
            m_TermMphfHeader = source.m_TermMphfHeader;
            m_TermMphfDisplacements = std::move(source.m_TermMphfDisplacements);
            m_TermMphfDisplacementCount = source.m_TermMphfDisplacementCount;
            m_TermMphfEntryPages = source.m_TermMphfEntryPages;
            m_TermMphfEntryPageCount = source.m_TermMphfEntryPageCount;
            source.m_TermMphfHeader = {};
            source.m_TermMphfDisplacementCount = 0;
            source.m_TermMphfEntryPages = nullptr;
            source.m_TermMphfEntryPageCount = 0;

            StartBlockThread(m_IndexPool);
            StartBlockThread(m_LeafTermPool);
        }

        /*
        * FindTermData — two-level lookup through HeadTermEntry + leaf term block.
        *
        * Step 1 — BloomFilter check (placeholder, always passes).
        * Step 2 — Level-1: binary search m_HeadTermEntries for the leaf block whose
        *           HTE_FirstTerm <= term.
        * Step 3 — Level-2: binary search directory offsets inside the leaf block.
        * Step 4 — Fill out-params from the LeafTermEntry and return true.
        */
        bool FindTermData(const char* term,
                          uint32_t*   indexBlockIDOut,
                          uint32_t*   indexOffsetOut,
                          uint32_t*   indexLengthOut,
                          uint32_t*   docFreqOut,
                          uint32_t*   continuationBlockCountOut = nullptr)
        {
            /* Step 1: BloomFilter */
            const size_t termLength = std::strlen(term);
            if (!m_BloomFilter.CanTermExist(term, termLength)) return false;
            if (HasTermMphf()
                && FindTermDataMphf(term, termLength, indexBlockIDOut, indexOffsetOut, indexLengthOut, docFreqOut, continuationBlockCountOut))
                return true;

            return FindTermDataHeadLeaf(term, termLength, indexBlockIDOut, indexOffsetOut, indexLengthOut, docFreqOut, continuationBlockCountOut);
        }

        void* GetBlock(BlockKind kind, uint32_t block_seq, uint32_t* slotOut, bool sequential = false)
        {
            BlockCachePool& pool = kind == BlockKind::Index ? m_IndexPool : m_LeafTermPool;
            if (sequential && pool.BCP_Pages && pool.BCP_LogicTable && pool.BCP_SlotTable && block_seq < pool.BCP_TotalBlockCount) {
                const uint32_t slot = pool.BCP_LogicTable[block_seq];
                if (slot != UINT32_MAX && slot < pool.BCP_SlotCount) {
                    ++pool.BCP_SlotTable[slot].Ref;
                    *slotOut = slot;
                    return pool.BCP_Pages + static_cast<size_t>(slot) * PAGE_SIZE;
                }
            }

            if (sequential && LoadSequentialWindow(pool, block_seq)) {
                const uint32_t slot = pool.BCP_LogicTable[block_seq];
                ++pool.BCP_SlotTable[slot].Ref;
                *slotOut = slot;
                return pool.BCP_Pages + static_cast<size_t>(slot) * PAGE_SIZE;
            }

            if (m_DirectBlockAccess) {
                BlockRequest request;
                request.Type = BlockRequestType::Get;
                request.BlockSeq = block_seq;
                ProcessGetBlock(pool, request);
                *slotOut = request.Slot;
                return request.Address;
            }

            BlockRequest request;
            request.Type = BlockRequestType::Get;
            request.BlockSeq = block_seq;
            {
                std::lock_guard<std::mutex> lock(pool.BCP_RequestMutex);
                pool.BCP_Requests.push_back(&request);
            }
            pool.BCP_RequestCv.notify_one();
            request.Completion.wait();
            *slotOut = request.Slot;
            return request.Address;
        }

        void ReleaseBlock(BlockKind kind, uint32_t slot, bool sequential = false)
        {
            if (slot == UINT32_MAX) return;
            BlockCachePool& pool = kind == BlockKind::Index ? m_IndexPool : m_LeafTermPool;
            if (sequential && pool.BCP_SlotTable && slot < pool.BCP_SlotCount && pool.BCP_SlotTable[slot].Ref > 0) {
                --pool.BCP_SlotTable[slot].Ref;
                return;
            }

            if (m_DirectBlockAccess) {
                BlockRequest request;
                request.Type = BlockRequestType::Release;
                request.Slot = slot;
                ProcessReleaseBlock(pool, request);
                return;
            }

            BlockRequest request;
            request.Type = BlockRequestType::Release;
            request.Slot = slot;
            {
                std::lock_guard<std::mutex> lock(pool.BCP_RequestMutex);
                pool.BCP_Requests.push_back(&request);
            }
            pool.BCP_RequestCv.notify_one();
            request.Completion.wait();
        }

        void SetBlockMemory(uint8_t* indexBlocks,
                    uint8_t* leafTermBlocks)
        {
            if (m_IndexPool.BCP_Pages != indexBlocks) {
                ExitBlockThread(m_IndexPool);
                if (m_IndexPool.BCP_Pages)
                    PinnedMemFree(m_IndexPool.BCP_Pages);
            }
            if (m_LeafTermPool.BCP_Pages != leafTermBlocks) {
                ExitBlockThread(m_LeafTermPool);
                if (m_LeafTermPool.BCP_Pages)
                    PinnedMemFree(m_LeafTermPool.BCP_Pages);
            }
            m_IndexPool.BCP_Pages = indexBlocks;
            m_LeafTermPool.BCP_Pages = leafTermBlocks;
            if (!m_IndexPool.BCP_Pages) {
                if (m_IndexPool.BCP_LogicTable)
                    PinnedMemFree(m_IndexPool.BCP_LogicTable);
                if (m_IndexPool.BCP_SlotTable)
                    PinnedMemFree(m_IndexPool.BCP_SlotTable);
                delete m_IndexPool.BCP_File;
                m_IndexPool.BCP_LogicTable = nullptr;
                m_IndexPool.BCP_SlotTable = nullptr;
                m_IndexPool.BCP_File = nullptr;
                m_IndexPool.BCP_TotalBlockCount = 0;
                m_IndexPool.BCP_SlotCount = 0;
                m_IndexPool.BCP_EvictSlot = 0;
                m_IndexPool.BCP_Requests.clear();
                m_IndexPool.BCP_ExitThread = false;
            }
            if (!m_LeafTermPool.BCP_Pages) {
                if (m_LeafTermPool.BCP_LogicTable)
                    PinnedMemFree(m_LeafTermPool.BCP_LogicTable);
                if (m_LeafTermPool.BCP_SlotTable)
                    PinnedMemFree(m_LeafTermPool.BCP_SlotTable);
                delete m_LeafTermPool.BCP_File;
                m_LeafTermPool.BCP_LogicTable = nullptr;
                m_LeafTermPool.BCP_SlotTable = nullptr;
                m_LeafTermPool.BCP_File = nullptr;
                m_LeafTermPool.BCP_TotalBlockCount = 0;
                m_LeafTermPool.BCP_SlotCount = 0;
                m_LeafTermPool.BCP_EvictSlot = 0;
                m_LeafTermPool.BCP_Requests.clear();
                m_LeafTermPool.BCP_ExitThread = false;
            }
            StartBlockThread(m_IndexPool);
            StartBlockThread(m_LeafTermPool);
            if (!indexBlocks && !leafTermBlocks)
                ClearTermMphf();
        }

        uint8_t* Init(BlockKind kind,
                  const char* path,
                  uint64_t baseOffset,
                  uint32_t blockCount,
                  uint32_t slotCount)
        {
            BlockCachePool* pool = kind == BlockKind::Index ? &m_IndexPool : &m_LeafTermPool;
            ExitBlockThread(*pool);
            if (pool->BCP_Pages) {
                PinnedMemFree(pool->BCP_Pages);
                pool->BCP_Pages = nullptr;
            }
            if (pool->BCP_LogicTable) {
                PinnedMemFree(pool->BCP_LogicTable);
                pool->BCP_LogicTable = nullptr;
            }
            if (pool->BCP_SlotTable) {
                PinnedMemFree(pool->BCP_SlotTable);
                pool->BCP_SlotTable = nullptr;
            }
            delete pool->BCP_File;
            pool->BCP_File = nullptr;
            pool->BCP_TotalBlockCount = 0;
            pool->BCP_SlotCount = 0;
            pool->BCP_EvictSlot = 0;
            pool->BCP_Requests.clear();
            pool->BCP_ExitThread = false;
            pool->BCP_BaseOffset = baseOffset;
            if (path && *path) {
                pool->BCP_File = new FileAccess(path);
                pool->BCP_File->Init();
            }
            pool->BCP_TotalBlockCount = blockCount;
            pool->BCP_SlotCount = std::min(slotCount, blockCount);
            pool->BCP_Pages = pool->BCP_SlotCount ? static_cast<uint8_t*>(PinnedMemAlloc(static_cast<uint64_t>(pool->BCP_SlotCount) * PAGE_SIZE)) : nullptr;
            pool->BCP_LogicTable = pool->BCP_TotalBlockCount ? static_cast<uint32_t*>(PinnedMemAlloc(static_cast<uint64_t>(pool->BCP_TotalBlockCount) * sizeof(uint32_t))) : nullptr;
            pool->BCP_SlotTable = pool->BCP_SlotCount ? static_cast<IndexSlotEntry*>(PinnedMemAlloc(static_cast<uint64_t>(pool->BCP_SlotCount) * sizeof(IndexSlotEntry))) : nullptr;

            for (uint32_t i = 0; i < pool->BCP_TotalBlockCount; ++i)
                pool->BCP_LogicTable[i] = UINT32_MAX;

            for (uint32_t slot = 0; slot < pool->BCP_SlotCount; ++slot) {
                pool->BCP_SlotTable[slot].BlockID = UINT32_MAX;
                pool->BCP_SlotTable[slot].Ref = 0;
            }

            for (uint32_t block = 0; block < pool->BCP_SlotCount; ++block) {
                pool->BCP_LogicTable[block] = block;
                pool->BCP_SlotTable[block].BlockID = block;
            }
            pool->BCP_EvictSlot = pool->BCP_SlotCount;
            StartBlockThread(*pool);
            return pool->BCP_Pages;
        }

        std::shared_ptr<ElementFilter>           m_ElementFilter;
        BlockCachePool                           m_IndexPool;
        BlockCachePool                           m_LeafTermPool;
        BloomFilter                              m_BloomFilter;

        TermMphfHeader                          m_TermMphfHeader;
        std::unique_ptr<int32_t[]>              m_TermMphfDisplacements;
        uint32_t                                m_TermMphfDisplacementCount = 0;
        uint8_t*                                m_TermMphfEntryPages = nullptr;
        uint32_t                                m_TermMphfEntryPageCount = 0;
        bool                                    m_TermMphfEnabled = true;
        bool                                    m_DirectBlockAccess = false;

        /* Level-1: fixed directory — (HTE_FirstTerm → HTE_LeafTermBlockID), sorted by HTE_FirstTerm */
        std::unique_ptr<HeadTermEntry[]>         m_HeadTermEntries;
        uint32_t                                 m_HeadTermEntryCount = 0;

    private:
        bool FindTermDataMphf(const char* term,
                              size_t termLength,
                              uint32_t* indexBlockIDOut,
                              uint32_t* indexOffsetOut,
                              uint32_t* indexLengthOut,
                              uint32_t* docFreqOut,
                              uint32_t* continuationBlockCountOut) const
        {
            if (!m_TermMphfEntryPages || !m_TermMphfDisplacements || m_TermMphfHeader.TMH_TermCount == 0)
                return false;
            if (m_TermMphfHeader.TMH_Magic != TERM_MPHF_MAGIC
                || m_TermMphfHeader.TMH_BucketCount == 0
                || m_TermMphfHeader.TMH_SlotCount == 0
                || m_TermMphfDisplacementCount != m_TermMphfHeader.TMH_BucketCount)
                return false;

            const uint64_t bucket = TermMphfHash(term, termLength, m_TermMphfHeader.TMH_BucketSeed) % m_TermMphfHeader.TMH_BucketCount;
            const int64_t displacement = m_TermMphfDisplacements[static_cast<size_t>(bucket)];
            const uint64_t slot = displacement < 0
                ? static_cast<uint64_t>(-displacement - 1)
                : TermMphfHash(term, termLength, TermMphfSlotSeed(m_TermMphfHeader.TMH_SlotSeed, static_cast<uint32_t>(displacement))) % m_TermMphfHeader.TMH_SlotCount;
            if (slot >= m_TermMphfHeader.TMH_SlotCount) return false;
            const uint64_t entriesPerPage = PAGE_SIZE / sizeof(TermMphfEntry);
            const uint64_t pageId = slot / entriesPerPage;
            const uint64_t inPage = slot % entriesPerPage;
            if (pageId >= m_TermMphfEntryPageCount)
                return false;

            const auto* pageBase = m_TermMphfEntryPages + pageId * PAGE_SIZE;
            const auto* entry = reinterpret_cast<const TermMphfEntry*>(pageBase + inPage * sizeof(TermMphfEntry));
            uint64_t fingerprint = TermMphfHash(term, termLength, m_TermMphfHeader.TMH_FingerprintSeed);
            if (fingerprint == 0) fingerprint = 1;
            if (entry->LTE_Fingerprint != fingerprint)
                return false;

            *docFreqOut = entry->LTE_DocFreq;
            *indexBlockIDOut = entry->LTE_IndexBlockID;
            *indexOffsetOut = entry->LTE_IndexOffset;
            *indexLengthOut = entry->LTE_IndexLength;
            if (continuationBlockCountOut) *continuationBlockCountOut = entry->LTE_ContinuationBlockCount;
            return true;
        }

        bool FindTermDataHeadLeaf(const char* term,
                                  size_t termLength,
                                  uint32_t* indexBlockIDOut,
                                  uint32_t* indexOffsetOut,
                                  uint32_t* indexLengthOut,
                                  uint32_t* docFreqOut,
                                  uint32_t* continuationBlockCountOut) const
        {
            if (!m_HeadTermEntries || m_HeadTermEntryCount == 0) return false;
            if (termLength > HEAD_TERM_KEY_MAX) return false;

            const std::string_view termText(term, termLength);
            const HeadTermEntry* begin = m_HeadTermEntries.get();
            const HeadTermEntry* end = begin + m_HeadTermEntryCount;
            auto it = std::upper_bound(begin, end, termText,
                [](std::string_view t, const HeadTermEntry& e) { return t < std::string_view(e.HTE_FirstTerm, e.HTE_FirstTermLength); });
            if (it == begin) return false;
            --it;

            uint32_t leafSlot = UINT32_MAX;
            const LeafTermBlock* block = static_cast<const LeafTermBlock*>(const_cast<IndexBlockTable*>(this)->GetBlock(BlockKind::LeafTerm, it->HTE_LeafTermBlockID, &leafSlot));
            if (!block) return false;
            const uint8_t* blockBase = reinterpret_cast<const uint8_t*>(block);
            const uint32_t entryCount = block->LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1];

            uint32_t left = 0;
            uint32_t right = entryCount;
            while (left < right) {
                const uint32_t mid = left + (right - left) / 2;
                const LeafTermEntry* entry = reinterpret_cast<const LeafTermEntry*>(blockBase + block->LTB_Directory[mid]);
                const std::string_view entryTerm(entry->LTE_Term, entry->LTE_TermLength);
                if (entryTerm < termText)
                    left = mid + 1;
                else
                    right = mid;
            }

            if (left == entryCount) {
                const_cast<IndexBlockTable*>(this)->ReleaseBlock(BlockKind::LeafTerm, leafSlot);
                return false;
            }
            const LeafTermEntry* entry = reinterpret_cast<const LeafTermEntry*>(blockBase + block->LTB_Directory[left]);
            const std::string_view entryTerm(entry->LTE_Term, entry->LTE_TermLength);
            if (entryTerm != termText) {
                const_cast<IndexBlockTable*>(this)->ReleaseBlock(BlockKind::LeafTerm, leafSlot);
                return false;
            }

            *docFreqOut = entry->LTE_DocFreq;
            *indexBlockIDOut = entry->LTE_IndexBlockID;
            *indexOffsetOut = entry->LTE_IndexOffset;
            *indexLengthOut = entry->LTE_IndexLength;
            if (continuationBlockCountOut) *continuationBlockCountOut = entry->LTE_ContinuationBlockCount;
            const_cast<IndexBlockTable*>(this)->ReleaseBlock(BlockKind::LeafTerm, leafSlot);
            return true;
        }

        bool HasTermMphf() const
        {
            return m_TermMphfEntryPages
                && m_TermMphfDisplacements
                && m_TermMphfEnabled
                && m_TermMphfHeader.TMH_Magic == TERM_MPHF_MAGIC
                && m_TermMphfHeader.TMH_TermCount > 0
                && m_TermMphfHeader.TMH_BucketCount > 0
                && m_TermMphfHeader.TMH_SlotCount > 0
                && m_TermMphfDisplacementCount == m_TermMphfHeader.TMH_BucketCount;
        }

        void ClearTermMphf()
        {
            if (m_TermMphfEntryPages)
                PinnedMemFree(m_TermMphfEntryPages);
            m_TermMphfHeader = {};
            m_TermMphfDisplacements.reset();
            m_TermMphfDisplacementCount = 0;
            m_TermMphfEntryPages = nullptr;
            m_TermMphfEntryPageCount = 0;
        }

        void HandOverPool(BlockCachePool& dest, BlockCachePool& source)
        {
            dest.BCP_Pages = source.BCP_Pages;
            dest.BCP_BaseOffset = source.BCP_BaseOffset;
            dest.BCP_TotalBlockCount = source.BCP_TotalBlockCount;
            dest.BCP_SlotCount = source.BCP_SlotCount;
            dest.BCP_EvictSlot = source.BCP_EvictSlot;
            dest.BCP_LogicTable = source.BCP_LogicTable;
            dest.BCP_SlotTable = source.BCP_SlotTable;
            dest.BCP_File = source.BCP_File;
            dest.BCP_Requests.clear();
            dest.BCP_ExitThread = false;

            source.BCP_Pages = nullptr;
            source.BCP_BaseOffset = 0;
            source.BCP_TotalBlockCount = 0;
            source.BCP_SlotCount = 0;
            source.BCP_EvictSlot = 0;
            source.BCP_LogicTable = nullptr;
            source.BCP_SlotTable = nullptr;
            source.BCP_File = nullptr;
            source.BCP_Requests.clear();
            source.BCP_ExitThread = false;
        }

        void StartBlockThread(BlockCachePool& pool)
        {
            if (m_DirectBlockAccess)
                return;
            if (pool.BCP_Pages && pool.BCP_SlotCount && !pool.BCP_Thread.joinable()) {
                pool.BCP_ExitThread = false;
                BlockCachePool* target = &pool;
                pool.BCP_Thread = std::thread([this, target]() { BlockThreadMain(*target); });
            }
        }

        void ExitBlockThread(BlockCachePool& pool)
        {
            if (!pool.BCP_Thread.joinable()) return;
            {
                std::lock_guard<std::mutex> lock(pool.BCP_RequestMutex);
                pool.BCP_ExitThread = true;
            }
            pool.BCP_RequestCv.notify_one();
            pool.BCP_Thread.join();
        }

        bool LoadSequentialWindow(BlockCachePool& pool, uint32_t startBlock)
        {
            if (!pool.BCP_File || !pool.BCP_Pages || !pool.BCP_LogicTable || !pool.BCP_SlotTable)
                return false;
            if (startBlock >= pool.BCP_TotalBlockCount || pool.BCP_SlotCount == 0)
                return false;

            const uint32_t blockCount = std::min(pool.BCP_SlotCount, pool.BCP_TotalBlockCount - startBlock);

            for (uint32_t slot = 0; slot < pool.BCP_SlotCount; ++slot) {
                if (pool.BCP_SlotTable[slot].Ref > 0)
                    return false;
            }

            for (uint32_t slot = 0; slot < pool.BCP_SlotCount; ++slot) {
                if (pool.BCP_SlotTable[slot].BlockID != UINT32_MAX)
                    pool.BCP_LogicTable[pool.BCP_SlotTable[slot].BlockID] = UINT32_MAX;
                pool.BCP_SlotTable[slot].BlockID = UINT32_MAX;
                pool.BCP_SlotTable[slot].Ref = 0;
            }

            const uint64_t bytes = static_cast<uint64_t>(blockCount) * PAGE_SIZE;
            if (bytes > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                return false;
            if (!pool.BCP_File->SetPosition(pool.BCP_BaseOffset + static_cast<uint64_t>(startBlock) * PAGE_SIZE)
                || pool.BCP_File->GetData(pool.BCP_Pages, static_cast<int>(bytes)) != static_cast<int>(bytes)) {
                return false;
            }

            for (uint32_t offset = 0; offset < blockCount; ++offset) {
                const uint32_t block = startBlock + offset;
                const uint32_t slot = offset;
                pool.BCP_SlotTable[slot].BlockID = block;
                pool.BCP_SlotTable[slot].Ref = 0;
                pool.BCP_LogicTable[block] = slot;
            }

            pool.BCP_EvictSlot = blockCount;

            return pool.BCP_LogicTable[startBlock] != UINT32_MAX;
        }

        void BlockThreadMain(BlockCachePool& pool)
        {
            while (true) {
                BlockRequest* request = nullptr;
                {
                    std::unique_lock<std::mutex> lock(pool.BCP_RequestMutex);
                    while (!pool.BCP_ExitThread && pool.BCP_Requests.empty())
                        pool.BCP_RequestCv.wait(lock);
                    if (pool.BCP_ExitThread && pool.BCP_Requests.empty())
                        break;
                    request = pool.BCP_Requests.front();
                    pool.BCP_Requests.pop_front();
                }

                if (request->Type == BlockRequestType::Get)
                    ProcessGetBlock(pool, *request);
                else if (request->Type == BlockRequestType::Release)
                    ProcessReleaseBlock(pool, *request);

                request->Completion.count_down();
            }
        }

        void ProcessGetBlock(BlockCachePool& pool, BlockRequest& request)
        {
            uint32_t block_seq = request.BlockSeq;
            uint32_t slot = pool.BCP_LogicTable[block_seq];
            if (slot != UINT32_MAX) {
                ++pool.BCP_SlotTable[slot].Ref;
                request.Slot = slot;
                request.Address = pool.BCP_Pages + static_cast<size_t>(slot) * PAGE_SIZE;
                return;
            }

            uint32_t found = UINT32_MAX;
            for (uint32_t scanned = 0; scanned < pool.BCP_SlotCount; ++scanned) {
                uint32_t candidate = pool.BCP_EvictSlot++ % pool.BCP_SlotCount;
                if (pool.BCP_SlotTable[candidate].Ref == 0) {
                    found = candidate;
                    break;
                }
            }
            if (found == UINT32_MAX) return;

            uint32_t oldBlock = pool.BCP_SlotTable[found].BlockID;
            if (oldBlock != UINT32_MAX)
                pool.BCP_LogicTable[oldBlock] = UINT32_MAX;

            void* address = pool.BCP_Pages + static_cast<size_t>(found) * PAGE_SIZE;
            if (!pool.BCP_File->ReadBlock(block_seq, address, PAGE_SIZE, pool.BCP_BaseOffset)) {
                pool.BCP_SlotTable[found].BlockID = UINT32_MAX;
                pool.BCP_SlotTable[found].Ref = 0;
                return;
            }

            pool.BCP_SlotTable[found].BlockID = block_seq;
            pool.BCP_SlotTable[found].Ref = 1;
            pool.BCP_LogicTable[block_seq] = found;
            request.Slot = found;
            request.Address = address;
        }

        void ProcessReleaseBlock(BlockCachePool& pool, BlockRequest& request)
        {
            uint32_t slot = request.Slot;
            if (slot != UINT32_MAX && pool.BCP_SlotTable[slot].Ref > 0)
                --pool.BCP_SlotTable[slot].Ref;
        }

};

constexpr uint64_t MAX_DOCID       = UINT64_MAX;
constexpr uint32_t MAX_BLOCK_SIZE  = PAGE_SIZE;

#endif
