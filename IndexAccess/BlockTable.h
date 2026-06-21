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
#include <condition_variable>
#include <deque>
#include <latch>
#include <mutex>
#include "../Utils/FileAccess.h"
#include "ElementFilter.h"
#include "MemOperation.h"

static constexpr int PAGE_SIZE  = 4096;
static constexpr size_t DOC_REC_SIZE = 1024;
static constexpr size_t DOC_VECTOR_DIM = 512;
static constexpr size_t DOC_VECTOR_STORAGE_MAX_DIM = DOC_VECTOR_DIM;  // fixed int8[512]
static constexpr size_t DOC_PATH_MAX = 256;
static constexpr size_t HEAD_TERM_KEY_MAX = 26;
static constexpr size_t LEAF_TERM_DIRECTORY_COUNT = 96;
static constexpr size_t LEAF_TERM_DATA_OFFSET = LEAF_TERM_DIRECTORY_COUNT * sizeof(uint16_t);
static constexpr uint8_t  INDEX_FILE_MAGIC[8] = {'M','O','O','N','S','H','O','T'};
static constexpr uint32_t INDEX_FORMAT_VERSION = 12;

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

#pragma pack(push,1)
struct IndexBlockContinuationHeader {
    uint64_t IBCH_MaxDocID;
    uint32_t IBCH_DataLength;
};
#pragma pack(pop)

struct alignas(8) IndexBlock {
    uint8_t IB_Data[PAGE_SIZE];
};

#pragma pack(push,1)
struct LeafTermEntry {
    uint32_t    LTE_DocFreq                 = 0;
    uint32_t    LTE_IndexBlockID            = 0;
    uint32_t    LTE_IndexOffset             = 0;
    uint32_t    LTE_IndexLength             = 0;
    uint32_t    LTE_ContinuationBlockCount  = 0;
    uint32_t    LTE_Flags                   = 0;
    uint8_t     LTE_TermLength              = 0;
    char        LTE_Term[0];
};
#pragma pack(pop)

struct alignas(16) HeadTermEntry {
    uint32_t HTE_LeafTermBlockID = 0;
    uint16_t HTE_FirstTermLength = 0;
    char     HTE_FirstTerm[HEAD_TERM_KEY_MAX] = {};
};

/*
* LeafTermBlock is one 4096-byte page.
* LTB_Directory[0..94] stores LeafTermEntry offsets from the block base.
* LTB_Directory[95] stores the number of entries in this block.
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
        }

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
            uint32_t leafSlot = UINT32_MAX;
            const LeafTermBlock* block = static_cast<const LeafTermBlock*>(GetBlock(BlockKind::LeafTerm, blockID, &leafSlot));
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
                ReleaseBlock(BlockKind::LeafTerm, leafSlot);
                return false;
            }
            const LeafTermEntry* entry = reinterpret_cast<const LeafTermEntry*>(blockBase + block->LTB_Directory[left]);
            const std::string_view entryTerm(entry->LTE_Term, entry->LTE_TermLength);
            if (entryTerm != termText) {
                ReleaseBlock(BlockKind::LeafTerm, leafSlot);
                return false;
            }

            *docFreqOut = entry->LTE_DocFreq;
            *indexBlockIDOut = entry->LTE_IndexBlockID;
            *indexOffsetOut = entry->LTE_IndexOffset;
            *indexLengthOut = entry->LTE_IndexLength;
            if (continuationBlockCountOut) *continuationBlockCountOut = entry->LTE_ContinuationBlockCount;
            ReleaseBlock(BlockKind::LeafTerm, leafSlot);
            return true;
        }

        void* GetBlock(BlockKind kind, uint32_t block_seq, uint32_t* slotOut)
        {
            BlockRequest request;
            request.Type = BlockRequestType::Get;
            request.BlockSeq = block_seq;
            BlockCachePool& pool = kind == BlockKind::Index ? m_IndexPool : m_LeafTermPool;
            {
                std::lock_guard<std::mutex> lock(pool.BCP_RequestMutex);
                pool.BCP_Requests.push_back(&request);
            }
            pool.BCP_RequestCv.notify_one();
            request.Completion.wait();
            *slotOut = request.Slot;
            return request.Address;
        }

        void ReleaseBlock(BlockKind kind, uint32_t slot)
        {
            BlockRequest request;
            request.Type = BlockRequestType::Release;
            request.Slot = slot;
            BlockCachePool& pool = kind == BlockKind::Index ? m_IndexPool : m_LeafTermPool;
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
            if (m_IndexPool.BCP_Pages && m_IndexPool.BCP_SlotCount && !m_IndexPool.BCP_Thread.joinable()) {
                m_IndexPool.BCP_ExitThread = false;
                m_IndexPool.BCP_Thread = std::thread([this]() { BlockThreadMain(m_IndexPool); });
            }
            if (m_LeafTermPool.BCP_Pages && m_LeafTermPool.BCP_SlotCount && !m_LeafTermPool.BCP_Thread.joinable()) {
                m_LeafTermPool.BCP_ExitThread = false;
                m_LeafTermPool.BCP_Thread = std::thread([this]() { BlockThreadMain(m_LeafTermPool); });
            }
        }

        void Init(BlockKind kind,
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
        }

        std::shared_ptr<ElementFilter>           m_ElementFilter;
        BlockCachePool                           m_IndexPool;
        BlockCachePool                           m_LeafTermPool;
        BloomFilter                              m_BloomFilter;

        /* Level-1: fixed directory — (HTE_FirstTerm → HTE_LeafTermBlockID), sorted by HTE_FirstTerm */
        std::unique_ptr<HeadTermEntry[]>         m_HeadTermEntries;
        uint32_t                                 m_HeadTermEntryCount = 0;

    private:
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
