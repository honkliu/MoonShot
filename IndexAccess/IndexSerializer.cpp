/*
 * IndexSerializer — v14 paged Head/Leaf term table plus fixed-entry MPHF.
 *
 * Block IB_Data: raw posting bytes only (no key/doc_freq/data_len headers).
 * Head/Leaf term table: two-level term metadata structure.
 *   Level-1 head table: fixed sorted HeadTermEntry array.
 *   Level-2 leaf blocks: 4096-byte pages with directory offsets and packed entries.
 * A term's continuation segment starts at byte 0 of the next block and carries
 * IndexBlockContinuationHeader; the final continuation block can reuse its tail
 * for later term starts.
 */

#include "IndexSerializer.h"
#include "BlockTable.h"
#include "FileAccess.h"

#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cassert>
#include <limits>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static bool read_vbc_pair_end(const uint8_t* data, size_t size, size_t& offset)
{
    auto read_one = [&]() -> bool {
        while (offset < size) {
            const uint8_t byte = data[offset++];
            if ((byte & 0x80u) == 0)
                return true;
        }
        return false;
    };
    return read_one() && read_one();
}

static size_t posting_prefix_bytes(const uint8_t* data, size_t size, size_t capacity)
{
    size_t cursor = 0;
    size_t lastPairEnd = 0;
    const size_t limit = std::min(size, capacity);
    while (cursor < limit) {
        const size_t before = cursor;
        if (!read_vbc_pair_end(data, size, cursor) || cursor > limit) {
            cursor = before;
            break;
        }
        lastPairEnd = cursor;
    }
    return lastPairEnd;
}

struct TermMphfBuildTerm {
    const std::string* Term = nullptr;
    std::string OwnedTerm;
    uint32_t DocFreq = 0;
    uint32_t IndexBlockID = 0;
    uint32_t IndexOffset = 0;
    uint32_t IndexLength = 0;
    uint32_t ContinuationBlockCount = 0;
    uint32_t Flags = 0;
};

static const std::string& mphf_term_text(const TermMphfBuildTerm& term)
{
    return term.Term ? *term.Term : term.OwnedTerm;
}

static uint32_t next_power_of_two(uint32_t value)
{
    if (value <= 1) return 1;
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return value + 1;
}

static uint32_t mphf_slot_for(const std::string& term, uint32_t slotCount, uint64_t slotSeed, uint32_t displacement)
{
    return static_cast<uint32_t>(TermMphfHash(term.data(), term.size(), TermMphfSlotSeed(slotSeed, displacement)) % slotCount);
}

static void clear_term_mphf(BuildBlocksResult& res)
{
    res.BBR_TermMphfHeader = {};
    res.BBR_TermMphfDisplacements.clear();
    res.BBR_TermMphfEntryPages.clear();
}

static bool try_build_term_mphf(const std::vector<TermMphfBuildTerm>& terms,
                                BuildBlocksResult& res,
                                uint64_t bucketSeed,
                                uint64_t slotSeed,
                                uint64_t fingerprintSeed)
{
    const uint32_t termCount = static_cast<uint32_t>(terms.size());
    const uint32_t bucketCount = next_power_of_two(std::max<uint32_t>(1, termCount / 4));
    const uint32_t slotCount = termCount;
    const uint32_t maxDisplacement = std::min<uint32_t>(1u << 20, std::max<uint32_t>(4096u, std::max<uint32_t>(1u, slotCount / 16)));

    std::vector<uint32_t> ids(termCount);
    for (uint32_t i = 0; i < termCount; ++i) ids[i] = i;
    std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
        return mphf_term_text(terms[a]) < mphf_term_text(terms[b]);
    });
    for (uint32_t i = 1; i < termCount; ++i) {
        if (mphf_term_text(terms[ids[i - 1]]) == mphf_term_text(terms[ids[i]]))
            return false;
    }

    std::vector<std::vector<uint32_t>> buckets(bucketCount);
    for (uint32_t i = 0; i < termCount; ++i) {
        const auto& term = mphf_term_text(terms[i]);
        const uint32_t bucket = static_cast<uint32_t>(TermMphfHash(term.data(), term.size(), bucketSeed) % bucketCount);
        buckets[bucket].push_back(i);
    }

    std::vector<uint32_t> order(bucketCount);
    for (uint32_t i = 0; i < bucketCount; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return buckets[a].size() > buckets[b].size();
    });

    std::vector<int32_t> displacements(bucketCount, -1);
    std::vector<uint8_t> used(slotCount, 0);
    std::vector<uint32_t> slots(termCount, UINT32_MAX);

    for (uint32_t bucket : order) {
        const auto& bucketTerms = buckets[bucket];
        if (bucketTerms.empty()) {
            displacements[bucket] = 0;
            continue;
        }

        bool placed = false;
        std::vector<uint32_t> candidateSlots(bucketTerms.size());
        for (uint32_t displacement = 0; displacement < maxDisplacement && !placed; ++displacement) {
            bool ok = true;
            for (size_t i = 0; i < bucketTerms.size(); ++i) {
                const auto& term = mphf_term_text(terms[bucketTerms[i]]);
                const uint32_t slot = mphf_slot_for(term, slotCount, slotSeed, displacement);
                candidateSlots[i] = slot;
                if (used[slot]) {
                    ok = false;
                    break;
                }
                for (size_t j = 0; j < i; ++j) {
                    if (candidateSlots[j] == slot) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;
            }

            if (!ok) continue;
            displacements[bucket] = static_cast<int32_t>(displacement);
            for (size_t i = 0; i < bucketTerms.size(); ++i) {
                used[candidateSlots[i]] = 1;
                slots[bucketTerms[i]] = candidateSlots[i];
            }
            placed = true;
        }

        if (!placed) {
            return false;
        }
    }

    const uint32_t entriesPerPage = PAGE_SIZE / sizeof(TermMphfEntry);
    const uint32_t pageCount = (slotCount + entriesPerPage - 1) / entriesPerPage;
    res.BBR_TermMphfEntryPages.assign(pageCount, IndexBlock{});

    for (uint32_t i = 0; i < termCount; ++i) {
        const uint32_t slot = slots[i];
        if (slot == UINT32_MAX) {
            clear_term_mphf(res);
            return false;
        }

        const uint32_t pageId = slot / entriesPerPage;
        const uint32_t inPage = slot % entriesPerPage;
        auto* pageBase = reinterpret_cast<uint8_t*>(&res.BBR_TermMphfEntryPages[pageId]);
        auto* entry = reinterpret_cast<TermMphfEntry*>(pageBase + inPage * sizeof(TermMphfEntry));
        const auto& source = terms[i];
        const auto& term = mphf_term_text(source);
        entry->LTE_DocFreq = source.DocFreq;
        entry->LTE_IndexBlockID = source.IndexBlockID;
        entry->LTE_IndexOffset = source.IndexOffset;
        entry->LTE_IndexLength = source.IndexLength;
        entry->LTE_ContinuationBlockCount = source.ContinuationBlockCount;
        entry->LTE_Flags = source.Flags;
        entry->LTE_Fingerprint = TermMphfHash(term.data(), term.size(), fingerprintSeed);
        if (entry->LTE_Fingerprint == 0) entry->LTE_Fingerprint = 1;
    }

    res.BBR_TermMphfHeader.TMH_Magic = TERM_MPHF_MAGIC;
    res.BBR_TermMphfHeader.TMH_TermCount = termCount;
    res.BBR_TermMphfHeader.TMH_BucketCount = bucketCount;
    res.BBR_TermMphfHeader.TMH_SlotCount = slotCount;
    res.BBR_TermMphfHeader.TMH_BucketSeed = bucketSeed;
    res.BBR_TermMphfHeader.TMH_SlotSeed = slotSeed;
    res.BBR_TermMphfHeader.TMH_FingerprintSeed = fingerprintSeed;
    res.BBR_TermMphfDisplacements = std::move(displacements);
    return true;
}

static void build_term_mphf(const std::vector<TermMphfBuildTerm>& terms, BuildBlocksResult& res)
{
    clear_term_mphf(res);
    if (terms.empty()) return;

    constexpr uint64_t bucketSeedBase = 0x9ae16a3b2f90404full;
    constexpr uint64_t slotSeedBase = 0xc3a5c85c97cb3127ull;
    constexpr uint64_t fingerprintSeedBase = 0xb492b66fbe98f273ull;
    constexpr uint64_t seedStep = 0x9e3779b97f4a7c15ull;

    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint64_t bucketSeed = bucketSeedBase + seedStep * attempt;
        const uint64_t slotSeed = slotSeedBase ^ (seedStep * (attempt + 1));
        const uint64_t fingerprintSeed = fingerprintSeedBase + seedStep * (attempt + 3);
        if (try_build_term_mphf(terms, res, bucketSeed, slotSeed, fingerprintSeed))
            return;
        clear_term_mphf(res);
    }
}

void IndexSerializer::BuildTermMphfFromLeafBlocks(const LeafTermBlock* leafBlocks,
                                                  uint32_t leafBlockCount,
                                                  BuildBlocksResult& result)
{
    result.BBR_TermMphfHeader = {};
    result.BBR_TermMphfDisplacements.clear();
    result.BBR_TermMphfEntryPages.clear();

    if (!leafBlocks || leafBlockCount == 0) return;

    std::vector<TermMphfBuildTerm> terms;
    for (uint32_t blockIndex = 0; blockIndex < leafBlockCount; ++blockIndex) {
        const auto& block = leafBlocks[blockIndex];
        const uint32_t entryCount = std::min<uint32_t>(block.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1], LEAF_TERM_DIRECTORY_COUNT - 1);
        const auto* blockBase = reinterpret_cast<const uint8_t*>(&block);
        for (uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
            const uint16_t offset = block.LTB_Directory[entryIndex];
            if (offset < LEAF_TERM_DATA_OFFSET || PAGE_SIZE - offset < sizeof(LeafTermEntry))
                continue;
            const auto* entry = reinterpret_cast<const LeafTermEntry*>(blockBase + offset);
            if (PAGE_SIZE - offset < sizeof(LeafTermEntry) + entry->LTE_TermLength)
                continue;

            TermMphfBuildTerm term{};
            term.OwnedTerm.assign(entry->LTE_Term, entry->LTE_TermLength);
            term.Term = nullptr;
            term.DocFreq = entry->LTE_DocFreq;
            term.IndexBlockID = entry->LTE_IndexBlockID;
            term.IndexOffset = entry->LTE_IndexOffset;
            term.IndexLength = entry->LTE_IndexLength;
            term.ContinuationBlockCount = entry->LTE_ContinuationBlockCount;
            term.Flags = entry->LTE_Flags;
            terms.push_back(std::move(term));
        }
    }

    build_term_mphf(terms, result);
}

/* ─────────────────────────────────────────────────────────────────────────
 * build_blocks
 *
 * 1. Pack posting bytes into IndexBlocks.
 *    First block: raw posting bytes at LTE_IndexOffset/LTE_IndexLength.
 *    Continuation segment: [IndexBlockContinuationHeader][posting bytes] at page start;
 *    the last continuation block may reuse remaining bytes for following terms.
 * 2. Build a flat sorted LeafTermEntry list (one LeafTermEntry per term).
 * 3. Organize into two-level Head/Leaf term table:
 *    HeadTermEntry: one entry per 4096-byte LeafTermBlock -> first term in block.
 *    LeafTermBlocks: variable entry count, never crossing 4096-byte block boundary.
 * ─────────────────────────────────────────────────────────────────────────*/
BuildBlocksResult IndexSerializer::BuildBlocks(const PostingStore& store)
{
    constexpr size_t   DATA_CAP = sizeof(IndexBlock::IB_Data);

    /* Sort terms alphabetically */
    std::vector<std::pair<const std::string*, const PostingList*>> terms;
    terms.reserve(store.AllPostings().size());
    for (const auto& [k, pl] : store.AllPostings())
        terms.push_back({&k, &pl});
    std::sort(terms.begin(), terms.end(),
              [](auto& a, auto& b){ return *a.first < *b.first; });

    BuildBlocksResult res;
    IndexBlock cur   = {};
    size_t     wptr  = 0;
    uint32_t   seq   = 0;

    LeafTermBlock leafBlock{};
    size_t leafWriteOffset = 0;
    uint32_t leafEntryCount = 0;
    char firstLeafTerm[HEAD_TERM_KEY_MAX] = {};
    uint16_t firstLeafTermLength = 0;
    std::vector<TermMphfBuildTerm> mphfTerms;
    mphfTerms.reserve(terms.size());

    auto flush_leaf_block = [&]() {
        if (leafEntryCount == 0) return;
        leafBlock.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = static_cast<uint16_t>(leafEntryCount);
        HeadTermEntry head{};
        head.HTE_LeafTermBlockID = static_cast<uint32_t>(res.BBR_LeafTermBlocks.size());
        head.HTE_FirstTermLength = firstLeafTermLength;
        std::memcpy(head.HTE_FirstTerm, firstLeafTerm, head.HTE_FirstTermLength);
        res.BBR_HeadTermEntries.push_back(head);
        res.BBR_LeafTermBlocks.push_back(leafBlock);
        leafBlock = LeafTermBlock{};
        leafWriteOffset = 0;
        leafEntryCount = 0;
        firstLeafTermLength = 0;
        std::memset(firstLeafTerm, 0, sizeof(firstLeafTerm));
    };

    auto write_leaf_entry = [&](const std::string& term,
                                uint32_t docFreq,
                                uint32_t indexBlockID,
                                uint32_t indexOffset,
                                uint32_t indexLength,
                                uint32_t continuationBlockCount,
                                uint32_t flags) {
        const uint8_t termLength = static_cast<uint8_t>(term.size());
        const size_t entryBytes = sizeof(LeafTermEntry) + termLength;
        if (leafEntryCount > 0
            && (leafEntryCount >= LEAF_TERM_DIRECTORY_COUNT - 1
                || leafWriteOffset + entryBytes > sizeof(leafBlock.LTB_Data)))
            flush_leaf_block();

        if (leafEntryCount == 0) {
            firstLeafTermLength = termLength;
            std::memcpy(firstLeafTerm, term.data(), termLength);
        }

        leafBlock.LTB_Directory[leafEntryCount] = static_cast<uint16_t>(LEAF_TERM_DATA_OFFSET + leafWriteOffset);
        LeafTermEntry* entry = reinterpret_cast<LeafTermEntry*>(leafBlock.LTB_Data + leafWriteOffset);
        entry->LTE_DocFreq = docFreq;
        entry->LTE_IndexBlockID = indexBlockID;
        entry->LTE_IndexOffset = indexOffset;
        entry->LTE_IndexLength = indexLength;
        entry->LTE_ContinuationBlockCount = continuationBlockCount;
        entry->LTE_Flags = flags;
        std::memset(entry->LTE_Reserved, 0, sizeof(entry->LTE_Reserved));
        entry->LTE_TermLength = termLength;
        std::memcpy(entry->LTE_Term, term.data(), termLength);
        leafWriteOffset += entryBytes;
        ++leafEntryCount;
        ++res.BBR_TotalTerms;
        TermMphfBuildTerm mphfTerm{};
        mphfTerm.Term = &term;
        mphfTerm.DocFreq = docFreq;
        mphfTerm.IndexBlockID = indexBlockID;
        mphfTerm.IndexOffset = indexOffset;
        mphfTerm.IndexLength = indexLength;
        mphfTerm.ContinuationBlockCount = continuationBlockCount;
        mphfTerm.Flags = flags;
        mphfTerms.push_back(std::move(mphfTerm));
    };

    auto flush = [&]() {
        res.BBR_IndexBlocks.push_back(cur);
        ++seq; cur = {}; wptr = 0;
    };

    for (const auto& [key_ptr, pl] : terms) {
        const std::string& key   = *key_ptr;
        if (key.size() > HEAD_TERM_KEY_MAX)
            continue;
        const auto         bytes = pl->GetBytes();
        if (bytes.empty()) continue;

        uint32_t       doc_freq  = pl->doc_freq();
        const uint8_t* src       = bytes.data();
        size_t         remaining = bytes.size();

        if (wptr >= DATA_CAP) flush();

        size_t data_offset = wptr;
        size_t data_here   = posting_prefix_bytes(src, remaining, DATA_CAP - wptr);
        if (data_here == 0) {
            flush();
            data_offset = wptr;
            data_here = posting_prefix_bytes(src, remaining, DATA_CAP);
            if (data_here == 0) continue;
        }
        bool   has_more    = (data_here < remaining);

        const uint32_t indexBlockID = seq;
        std::memcpy(cur.IB_Data + wptr, src, data_here);
        wptr      += data_here;
        src       += data_here;
        remaining -= data_here;
        uint32_t continuationBlockCount = 0;

        if (has_more) {
            flush();

            while (remaining > 0) {
                constexpr size_t CONT_HDR = sizeof(IndexBlockContinuationHeader);
                size_t cont_cap  = DATA_CAP - CONT_HDR;
                size_t cont_here = posting_prefix_bytes(src, remaining, cont_cap);
                if (cont_here == 0) break;
                bool   more_cont = (cont_here < remaining);
                size_t cont_cursor = 0;
                uint64_t cont_max_docid = 0;
                while (cont_cursor < cont_here) {
                    cont_max_docid = 0;
                    uint8_t shift = 0;
                    while (true) {
                        const uint8_t byte = src[cont_cursor++];
                        cont_max_docid |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
                        if ((byte & 0x80u) == 0) break;
                        shift += 7;
                    }
                    while (src[cont_cursor++] & 0x80u) {}
                }

                auto* header = reinterpret_cast<IndexBlockContinuationHeader*>(cur.IB_Data + wptr);
                header->IBCH_MaxDocID = cont_max_docid;
                header->IBCH_DataLength = static_cast<uint32_t>(cont_here);
                wptr += sizeof(IndexBlockContinuationHeader);
                std::memcpy(cur.IB_Data + wptr, src, cont_here); wptr += cont_here;
                src       += cont_here;
                remaining -= cont_here;

                ++continuationBlockCount;

                if (more_cont) flush();
            }
        }

        write_leaf_entry(key,
                         doc_freq,
                         indexBlockID,
                         static_cast<uint32_t>(data_offset),
                         static_cast<uint32_t>(data_here),
                         continuationBlockCount,
                         0u);
    }

    if (wptr > 0) flush();
    flush_leaf_block();
    build_term_mphf(mphfTerms, res);

    return res;
}

/* ── Save ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Save(const IndexFileHeader& header,
                           const IndexBlockTable& blockTable,
                           const uint8_t* docData,
                           const char* path)
{
    if (!path || !*path) return false;

    FileAccess file(path);
    if (!file.InitWrite()) return false;

    if (!file.PutData(&header, sizeof(header))) return false;

    if (header.IFH_HeadTermEntryCount > 0) {
        const uint64_t bytes = sizeof(HeadTermEntry) * header.IFH_HeadTermEntryCount;
        if (!file.PutData(blockTable.m_HeadTermEntries.get(), bytes)) return false;
    }

    if (header.IFH_LeafTermBlockCount > 0) {
        const uint64_t bytes = sizeof(LeafTermBlock) * header.IFH_LeafTermBlockCount;
        if (!file.PutData(blockTable.m_LeafTermPool.BCP_Pages, bytes)) return false;
    }

    if (header.IFH_NumDocuments > 0) {
        const uint64_t bytes = DOC_REC_SIZE * header.IFH_NumDocuments;
        if (!file.PutData(docData, bytes)) return false;
    }

    if (header.IFH_IndexBlockCount > 0) {
        const uint64_t bytes = sizeof(IndexBlock) * header.IFH_IndexBlockCount;
        if (!file.PutData(blockTable.m_IndexPool.BCP_Pages, bytes)) return false;
    }

    if (header.IFH_TermMphfHeaderCount > 0) {
        if (!file.PutData(&blockTable.m_TermMphfHeader, sizeof(TermMphfHeader))) return false;
    }

    if (header.IFH_TermMphfDisplacementCount > 0) {
        const uint64_t bytes = sizeof(int32_t) * header.IFH_TermMphfDisplacementCount;
        if (!file.PutData(blockTable.m_TermMphfDisplacements.get(), bytes)) return false;
    }

    if (header.IFH_TermMphfEntryPageCount > 0) {
        const uint64_t bytes = sizeof(IndexBlock) * header.IFH_TermMphfEntryPageCount;
        if (!file.PutData(blockTable.m_TermMphfEntryPages, bytes)) return false;
    }

    return true;
}

bool IndexSerializer::IsValidIndex(const char* path)
{
    if (!path || !*path) return false;
    FileAccess file(path);
    if (!file.Init()) return false;
    uint8_t magic[8] = {};
    if (file.GetData(magic, sizeof(magic)) != sizeof(magic)) return false;
    return std::memcmp(magic, INDEX_FILE_MAGIC, 8) == 0;
}


