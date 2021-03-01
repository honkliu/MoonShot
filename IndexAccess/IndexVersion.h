#pragma once

#include "Attributes.h"
#include "fileformats.h"
#include "Decoder.h"     // Note that auto_ptr can't use forward-declared types.
#include "indexdata.h"

#include "basic_types.h"
#include "IndexSortRank.h"
#include "MSNRank.h"
#include <memory>
#include <unordered_map>

#include <RangeOperation.h>
#include <MetaStreamIndexList.h>

// DocumentDataReader.h is not necessary, but enough things depend on having 
// it included here that removing it is painful.
#include "DocumentDataReader.h"  
#include "IndexProperties.h"
#include "common_types.h"
#include "indexwordreader.h"

class ISR;
class ISRWord;
class CMemLfpManager;


namespace IndexServer
{
    class StreamUpdateLookUpTable;
    class ISRFactory;
};

namespace DynamicRank
{
    class DocumentCandidate;
    class FilterController;
};

namespace PerDocIndex
{
    struct L2CompiledQuery;
};


struct ExtractorState;
struct Constraint;

struct IndexPartitionDataForEarlyTermination
{
    // Document index of an early termination threshold.
    // Zero means no ranking partition available.
    UInt64 m_lastDocDataIndex;

    // Best sort rank of the current rank partition.
    IndexSortRank m_bestSortRank;
};

// IndexVersion is an opaque class used as a handle on a particular
// version of an index. While a version is held open, a snapshot of
// the index is preserved so that consistent queries can be run,
// however some index implementations may mutate between versions,
// e.g. if new documents are added. IndexVersion is not threadsafe so
// a version should only be used one thread at a time.
class IndexVersion : boost::noncopyable
{
public:

    // Info of a doc.
    struct PerDocInfo
    {
        // 1-based index of this document in current perdoc index file.
        UInt32 m_docIndex;

        // DocId.
        UInt64 m_docId;

        // The start location of this document.
        UInt64 m_docStartLoc;

        // The end location of this document.
        UInt64 m_docEndLoc;

        // The first 8 bytes of domain hash.
        UInt64 m_domainHash;

        // ctor.
        PerDocInfo();
    };


    // To identity main PDI doc and domain PDI doc.
    // Will extend this struct if we do runtime join on more information for L2 ranking.
    struct PerDocId
    {
        // 1-based index of this document in current main perdoc index file.
        UInt32 m_docIndex;

        // DocId.
        UInt64 m_docId;

        // The first 8 bytes of domain hash.
        UInt64 m_domainHash;

        // ctor.
        PerDocId();
    };

    // Get document candidate instance with specified doc index. The life cycle of
    // document candidate instance is managed by index version itself.
    virtual const DynamicRank::DocumentCandidate* GenerateDocumentCandidate(const PerDocId& p_perDocId,
                                                                            const PerDocIndex::L2CompiledQuery** p_l2CompiledQueries,
                                                                            UInt32 p_numL2CompiledQuery,
                                                                            DynamicRank::FilterController& p_filterController,
                                                                            ExtractorState& p_extractorState,
                                                                            CMemLfpManager& p_memManager);

    // Get document candidate instance with specified doc url hash. The life cycle of
    // document candidate instance is managed by index version itself.
    virtual const DynamicRank::DocumentCandidate* GenerateDocumentCandidate(const aggregator::UrlHash16B& p_urlHash16B,
                                                                            const PerDocIndex::L2CompiledQuery** p_l2CompiledQueries,
                                                                            UInt32 p_numL2CompiledQuery,
                                                                            DynamicRank::FilterController& p_filterController,
                                                                            ExtractorState& p_extractorState,
                                                                            CMemLfpManager& p_memManager,
                                                                            UInt64& p_docId);

    // Help to combine N DocumentCandidates into one.
    virtual const DynamicRank::DocumentCandidate* CombineDocumentCandidates(const std::vector<const DynamicRank::DocumentCandidate*>& p_list);

    // Get allocator instance.
    indexserver::Allocator& GetAllocator();

    // Return true if this IndexVersion has docdata for all documents
    virtual bool HasDocData() const;

    // Return an identifier, local to this index version, for the
    // document currently pointed to by the endDoc ISR. It is an
    // error to call GetDocumentData when the current location of
    // endDoc is Constants::MAX_LOC, or with endDoc not open on the
    // EndDoc word. This identifier can be passed to GetDocumentData
    // and GetRank.
    UInt64 GetDocumentDataIndex(ISRWord* p_endDoc) const;

    // For a given document index, returns the last document index and the 
    // best static rank score of the current ranking partion that should be 
    // checked before an early termination is triggered.  p_currentDocDataIndex 
    // is a document index.  p_indexPartitionData is info for 
    // early termination that is written by this method.
    virtual void GetRankPartitionToTerminateEarly(UInt64 p_currentDocDataIndex,
                                                  IndexPartitionDataForEarlyTermination& p_indexPartitionData) const;

    // Open a decoder for the given word, returning a NULL shared_ptr if the
    // word wasn't found in this index version.  The resulting decoder must be
    // imported into an ISR, Sequence or other hierarchy via ISRFactory, etc.
    virtual std::auto_ptr<IndexServer::Decoder>
    OpenDecoder(const UInt8* p_word, UInt32 p_wordLength) = 0;

    // Return the document data core for a document, indicated by
    // p_documentDataIndex. The p_documentDataIndex is obtained from a call 
    // to GetDocumentDataIndex.
    DocumentDataCore* GetDocumentDataCore(UInt64 p_documentDataIndex) const;

    // Return a pointer to complete document data for a document, indicated by
    // p_documentDataIndex.  The p_documentDataIndex is obtained from a 
    // call to GetDocumentDataIndex.  This returns a blob of data, and it 
    // should not be cast to any structure.  This data should be interpreted 
    // on the DocumentDataSchema embedded into the index file. 
    void* GetDocumentData(UInt64 p_documentDataIndex) const;

    // Return the document data size for a document to which the endDoc
    // is pointing to. Using the endDoc, we get the documentIndex
    // Different sized documents are not mixed within an index, so usually
    // the size of any document in the index is same as the size of the
    // first document and that is the reason by default we take the
    // NULL endoc which translates to docindex 0.
    // But in case of IndexConcatenated file, we might have
    // different index files concatenated and each of those index files
    // could possibly have different document sizes, so that in case
    // giving the correct documentDataIndex( from the endDoc) is necessary
    // for correctness using the endDoc, we can figure the subindex in the
    // indexConcatenated and accordingly return the document data size.
    UInt32 GetDocumentDataSize(ISRWord* p_endDoc = NULL) const;

    // Returns the pointer to StreamUpdateTable.
    // This stream update table if not NULL will carry all the stream names 
    // that are present in the index file. Along with the stream names, 
    // it also has document bit array, which determines if a document has 
    // that particular stream or not.
    const IndexServer::StreamUpdateLookUpTable* GetStreamUpdateTable() const;

    // Return the MSN Rank for the document. The p_documentDataIndex is 
    // obtained from a call to GetDocumentDataIndex.
    MSNRank GetRank(UInt64 p_documentDataIndex) const;

    // Retrieve the static data associated with this index.
    const IndexData* GetIndexData() const;

    // Prefetch the words if necessary in preparation for a query
    // Return true if prefetch completed, false if only issued.
    bool PrefetchWords(const UInt8 p_numWords,
                       const UInt8* p_word[],
                       const UInt32 p_length[],
                       bool p_probeOnly,
                       bool *p_expensive = 0,
                       bool* p_disableWord = NULL);

    // Check if the IndexFile has its own bloom filter instead of sharing the Primary one.
    bool HasBloomFilter() const;

    // A quick check to see if a particular word can exist before opening it.
    bool CanTermExist(const UInt32 p_length, const UInt8 p_term[]) const;

    const char* GetFileName() const;

    // Indicates whether this IndexVersion can be closed (i.e. that no DDRs or
    // ISRs are still open).
    bool CanClose();

    // Get the properites of this IndexVersion.
    const IndexProperties& GetIndexProperties() const;

    // Restrict this IndexVersion to a set of doc.
    // For per-doc index, this IndexVersion can serve after restricted, this function will throw an std::exception if fails.
    // For other index, it is no-op.
    virtual void RestrictToDocSet(std::vector<IndexVersion::PerDocInfo>& p_docs, CMemLfpManager& p_memManager);

    // Get the PerDocInfo for a doc Id. Return false if the index can not provide such information 
    // for the doc Id.
    virtual bool GetDocInfo(UInt64 p_docId, IndexVersion::PerDocInfo& p_perDocInfo) const;

    // Deprecated. Should use GetIndexProperties() instead.
    bool IsTigerIndex() const;

    // On demand doc data access.
    // (1) Client do not need to open and close doc data reader.
    // (2) This code hide the open/close doc data reader.

    // Get doc data using word for a doc.
    UInt64 GetDocData(const std::string& p_metaWord, ISRWord* p_endDoc, bool& p_found);

    // Get doc data using doc data Id for a doc. Doc data Id will map to word.
    UInt64 GetDocData(const UInt32 p_docDataId, ISRWord* p_endDoc, bool& p_found);

    UInt64 GetDocDataNext(const std::string& p_metaWord, ISRWord* p_endDoc, bool& p_found);

    UInt64 GetDocDataNext(const UInt32 p_docDataId, ISRWord* p_endDoc, bool& p_found);

    // Clean before Close.
    void CleanBeforeClose();

    // Get the trace id of this IndexVersion.
    const char* GetTraceId() const;

    // Set the trace id of this IndexVersion.
    void SetTraceId(const char* p_traceId);

    // Initialize m_docDataReaders and m_wordToDDRs.
    void InitializeDocDataReaders();

protected:

    // Simple constructor, just initializes a data member.  
    // Protected because IndexVersions are handed out by calling Open.
    explicit IndexVersion(indexserver::Allocator& p_allocator);

    // Destructor asserts that there are no open ISRs or DDRs.
    virtual ~IndexVersion() = 0;

    void SetIndexData(const IndexData* p_indexData);

    // Increments open DDR count, to be used by subclasses for each DDR 
    // they open.
    void IncrementOpenDDR();

    // Decrements the open DDR count, to be used by subclasses for each DDR 
    // they close.
    void DecrementOpenDDR();

    class Index* m_baseIndex; // Will clean this up later.

    IndexProperties m_indexProperties;

    // Return true if initialized doc data readers (both m_docDataReaders and m_wordToDDRs).
    bool IsDocDataInitialized();

private:
    virtual UInt64 GetDocumentDataIndexInternal(ISRWord* p_endDoc) const = 0;

    virtual DocumentDataCore* GetDocumentDataCoreInternal(UInt64 p_documentDataIndex) const = 0;

    virtual void* GetDocumentDataInternal(UInt64 p_documentDataIndex) const = 0;

    virtual UInt32 GetDocumentDataSizeInternal(ISRWord* p_endDoc) const = 0;

    virtual const IndexServer::StreamUpdateLookUpTable* GetStreamUpdateTableInternal() const = 0;

    virtual MSNRank GetRankInternal(UInt64 p_documentDataIndex) const = 0;

    // If p_checkBloomfilter is false, it will try to avoid check bloomfilter before do real open.
    virtual ISRWord* OpenWordInternal(const UInt8* p_word, 
                                      UInt32 p_wordLength,
                                      bool p_checkBloomfilter) = 0;
                                      
    virtual ISRWord* OpenEndDocInternal() = 0;

    virtual ISRWord* OpenFirstWordInternal() = 0;

    virtual IndexWordReader* OpenWordReaderInternal() = 0;

    virtual bool PrefetchWordsInternal(const UInt8 p_numWords, 
                                       const UInt8* p_word[], 
                                       const UInt32 p_length[],
                                       bool p_probeOnly, 
                                       bool* p_expensive, 
                                       bool* p_disableWord) = 0;

    // Check if the IndexFile has its own bloom filter instead of sharing the Primary one.
    virtual bool HasBloomFilterInternal() const;

    virtual bool CanTermExistInternal(const UInt32 p_length, 
                                      const UInt8 p_term[]) const;

    virtual DocumentDataReader* 
    OpenDDRWordInternal(const UInt8* p_word, 
                        UInt32 p_wordLength,
                        bool p_searchInOnlyDocData,
                        bool p_forceOpen,
                        bool p_isMiniDocumentMode) = 0;

    // Number of DDRs provided by this IndexVersion that are currently open.
    UInt32 m_numOpenDDRs;

    // Statistics about this index version.
    const IndexData* m_indexData;

    // IndexVersion hold these DocumentDataReaders on demand so that
    // client do not need to open/release them explicitly.
    // Changed the map object to a smart pointer, and set map object when needed.
    // So we can reduce the cost of the map object's initialization.
    boost::scoped_ptr<std::unordered_map<std::string, DocumentDataReader*>> m_wordToDDRs;

    // Is any doc data opened, only this is true, the docdata readers array will be looped and released.
    // Pls set it to true whenever a doc data reader is opened.
    bool m_isAnyDocDataOpened;

    DocumentDataReader* m_docDataReaders[DocDataMetaWords::MetaWordCount];

    // The trace id.
    const char* m_traceId;

    indexserver::Allocator& m_allocator;

    friend class IndexServer::ISRFactory;
    friend class IndexWordReader;
    friend class DocumentDataReader;
};
