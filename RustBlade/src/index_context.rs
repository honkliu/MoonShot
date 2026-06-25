use std::fs;
use std::fs::File;
use std::io::{Read, Write};
use std::sync::{Arc, Mutex};
use crate::posting_store::PostingStore;
use crate::block_table::IndexBlockTable;
use crate::index_writer::AdvancedIndexWriter;
use crate::index_writer::IndexWriter;
use crate::eval_tree::{EvalTree, EvalNode};
use crate::advanced_reader::AdvancedIndexReader;
use crate::composite_readers::{AndIndexReader, OrIndexReader, NotIndexReader, VectorIndexReader};
use crate::index_reader::{IndexReader, ReaderDocumentIDValue};
use crate::compiler::IndexSearchCompiler;
use crate::tokenizer::{SmartTokenizer, Tokenizer};
use crate::serializer::{IndexSerializer, IndexFileHeader};
use crate::error::{Result, RustBladeError};
use crate::block_table::{DOC_PATH_MAX, DOC_REC_SIZE, DOC_VECTOR_DIM, INDEX_FILE_HEADER_SIZE, PAGE_SIZE, HEAD_TERM_KEY_MAX, INDEX_BLOCK_CONTINUATION_HEADER_SIZE, LEAF_TERM_DATA_OFFSET, LEAF_TERM_DIRECTORY_COUNT, HeadTermEntry, IndexBlock, IndexBlockContinuationHeader, LeafTermEntry, LeafTermBlock, PinnedBlock};
use crate::vector_index::{HnswIndex, VectorMetric, VectorSearchResult};
use crate::vector_index::build_hashed_embedding;

pub struct Document {
    pub doc_id: u64,
    pub path: String,
    pub url: String,
    pub title: String,
    pub body: String,
    pub anchor: String,
    pub meta: String,
    pub importance: f32,
}

impl Default for Document {
    fn default() -> Self {
        Self {
            doc_id: u64::MAX,
            path: String::new(),
            url: String::new(),
            title: String::new(),
            body: String::new(),
            anchor: String::new(),
            meta: String::new(),
            importance: 0.0,
        }
    }
}

/// Central factory — owns PostingStore, BlockTable and all search components.
/// Mirrors MoonShot's IndexContext.h.
#[allow(non_snake_case)]
pub struct IndexContext {
    m_Store:       Arc<Mutex<PostingStore>>,
    m_BlockTable: Arc<IndexBlockTable>,
    m_Tokenizer:   SmartTokenizer,
    m_Compiler:    IndexSearchCompiler,
    m_VectorIndex: HnswIndex,
    m_VectorBuilt: bool,
    m_IndexPath:  Option<String>,
    m_Built:       bool,
    m_LoadedFromDisk: bool,
    m_LoadDelta: bool,
    m_IndexFileHeader: IndexFileHeader,
    m_DocData: Vec<u8>,
    m_DeltaContext: Option<Box<IndexContext>>,
    m_WriteBlockTable: IndexBlockTable,
    m_WriteVectorIndex: HnswIndex,
    m_WriteIndexFileHeader: IndexFileHeader,
    m_WriteDocData: Vec<u8>,
}

#[allow(non_snake_case)]
impl IndexContext {
    pub fn new() -> Self { Self::with_path(None) }

    pub fn with_path(index_path: Option<String>) -> Self {
        Self::with_path_and_load_delta(index_path, true)
    }

    pub fn with_path_and_load_delta(index_path: Option<String>, load_delta: bool) -> Self {
        let store = Arc::new(Mutex::new(PostingStore::new()));
        let mut ctx = Self {
            m_Store:       Arc::clone(&store),
            m_BlockTable: Arc::new(IndexBlockTable::new(512)),
            m_Tokenizer:   SmartTokenizer::new(),
            m_Compiler:    IndexSearchCompiler::new(SmartTokenizer::new()),
            m_VectorIndex: HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine),
            m_VectorBuilt: false,
            m_IndexPath:  index_path.clone(),
            m_Built:       false,
            m_LoadedFromDisk: false,
            m_LoadDelta: load_delta,
            m_IndexFileHeader: IndexFileHeader::default(),
            m_DocData: Vec::new(),
            m_DeltaContext: None,
            m_WriteBlockTable: IndexBlockTable::new(512),
            m_WriteVectorIndex: HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine),
            m_WriteIndexFileHeader: IndexFileHeader::default(),
            m_WriteDocData: Vec::new(),
        };
        if let Some(ref path) = index_path {
            let _ = ctx.LoadIndex(path);
        }
        ctx
    }

    pub fn GetWriter(&self) -> AdvancedIndexWriter {
        AdvancedIndexWriter::new(Arc::clone(&self.m_Store))
    }

    pub fn AllocateDocumentID(&self) -> u64 {
        self.m_Store.lock().unwrap().AllDocStats().keys().copied().max().map(|docId| docId + 1).unwrap_or(0)
    }

    pub fn AddDocument(&mut self, doc: &Document, buildVector: bool) -> u64 {
        let docId = if doc.doc_id == u64::MAX { self.AllocateDocumentID() } else { doc.doc_id };
        let mut writer = self.GetWriter();

        let titleTokens = self.m_Tokenizer.Tokenize(&doc.title);
        let url = if doc.url.is_empty() { &doc.path } else { &doc.url };
        let urlTokens = self.m_Tokenizer.Tokenize(url);
        let anchorTokens = self.m_Tokenizer.Tokenize(&doc.anchor);
        let bodyTokens = self.m_Tokenizer.Tokenize(&doc.body);
        let metaTokens = self.m_Tokenizer.Tokenize(&doc.meta);

        writer.Write(titleTokens.clone(), docId, "Title");
        writer.Write(urlTokens.clone(), docId, "URL");
        writer.Write(anchorTokens.clone(), docId, "Anchor");
        writer.Write(bodyTokens.clone(), docId, "Body");
        writer.Write(metaTokens.clone(), docId, "Meta");
        writer.SetDocImportance(docId, doc.importance);

        if buildVector {
            let mut embeddingTokens = Vec::with_capacity(
                titleTokens.len() + urlTokens.len() + anchorTokens.len() + bodyTokens.len() + metaTokens.len());
            embeddingTokens.extend(titleTokens);
            embeddingTokens.extend(urlTokens);
            embeddingTokens.extend(anchorTokens);
            embeddingTokens.extend(bodyTokens);
            embeddingTokens.extend(metaTokens);
            writer.SetDocVector(docId, build_hashed_embedding(&embeddingTokens));
        }

        if !doc.path.is_empty() {
            writer.SetDocPath(docId, doc.path.clone());
        }

        self.m_Built = false;
        self.m_VectorBuilt = false;
        docId
    }

    pub fn GetTokenizer(&self) -> &SmartTokenizer { &self.m_Tokenizer }
    pub fn GetCompiler(&self)  -> &IndexSearchCompiler { &self.m_Compiler }
    pub fn GetStore(&self) -> Arc<Mutex<PostingStore>> { Arc::clone(&self.m_Store) }
    pub fn DocumentCount(&self) -> u64 { self.m_IndexFileHeader.IFH_NumDocuments }
    pub fn AvgDocLen(&self) -> f32 { self.m_IndexFileHeader.IFH_AvgDocLength }
    pub fn GetDocPath(&self, doc_id: u64) -> String { self.m_Store.lock().unwrap().GetDocPath(ReaderDocumentIDValue(doc_id)).to_string() }

    pub fn HasDelta(&self) -> bool {
        self.m_DeltaContext.as_ref().map(|delta| delta.DocumentCount() > 0).unwrap_or(false)
    }

    pub fn GetDeltaContext(&mut self) -> Option<&mut IndexContext> {
        if self.HasDelta() { self.m_DeltaContext.as_deref_mut() } else { None }
    }

    // ── Build (in-memory) ────────────────────────────────────────────────────

    /// Pack PostingStore entries into multi-term IndexBlocks (sorted alphabetically)
    /// and populate the Head/Leaf term table. Called lazily on first search.
    pub fn Build(&mut self) {
        if self.m_Built { return; }

        let store = self.m_Store.lock().unwrap();

        let table = Arc::get_mut(&mut self.m_BlockTable)
            .expect("BlockTable must have no other refs during build");

        let (header, newTable, vectorIndex, docData) = Self::BuildIndexData(&store, true);
        *table = newTable;
        self.m_VectorIndex = vectorIndex;
        self.m_VectorBuilt = true;
        self.m_IndexFileHeader = header;
        self.m_DocData = docData;
        self.m_Built = true;
    }

    fn EnsureBuilt(&mut self) {
        if !self.m_Built { self.Build(); }
    }

    // ── Search ───────────────────────────────────────────────────────────────

    pub fn Compile(&self, query: &str, stream_set: &str) -> EvalTree {
        self.m_Compiler.Compile(query, stream_set)
    }

    pub fn GetReaderForQuery(&mut self, query: &str, stream_set: &str)
        -> Box<dyn IndexReader>
    {
        self.EnsureBuilt();
        let tree = self.Compile(query, stream_set);
        self.BuildIndexReader(tree.root)
    }

    pub fn GetReader(&mut self, tree: EvalTree) -> Box<dyn IndexReader> {
        self.EnsureBuilt();
        if tree.HasTextQuery() && tree.HasVectorQuery() {
            let children = vec![
                self.BuildIndexReader(tree.root),
                self.BuildVectorIndexReader(&tree.vector_query, tree.vector_ef_search),
            ];
            return Box::new(OrIndexReader::new(children));
        }
        if tree.HasVectorQuery() {
            return self.BuildVectorIndexReader(&tree.vector_query, tree.vector_ef_search);
        }
        self.BuildIndexReader(tree.root)
    }

    pub fn GetStreamReader(&mut self, stream_key: &str) -> Box<dyn IndexReader> {
        self.EnsureBuilt();
        let docFreq = self.m_Store.lock().unwrap().DocFreq(stream_key);
        Box::new(AdvancedIndexReader::open(
            stream_key, Arc::clone(&self.m_BlockTable), docFreq))
    }

    pub fn CompileToVector(&self, query: &str) -> Vec<f32> {
        self.m_Compiler.CompileToVector(query)
    }

    pub fn VectorSearch(&mut self, query: &[f32], top_k: usize, ef_search: usize) -> Vec<VectorSearchResult> {
        self.EnsureVectorIndexBuilt();
        self.m_VectorIndex.Search(query, top_k, ef_search)
    }

    pub fn VectorCount(&mut self) -> usize { self.EnsureVectorIndexBuilt(); self.m_VectorIndex.Size() }
    pub fn VectorDimension(&self) -> usize { self.m_VectorIndex.Dimension() }

    // ── Persistence ─────────────────────────────────────────────────────────

    pub fn SaveIndex(&mut self, path: &str) -> Result<()> {
        let savingDeltaIndex = self.m_IndexPath.as_ref()
            .map(|index_path| path == Self::DeltaIndexPath(index_path))
            .unwrap_or(false);
        if !savingDeltaIndex {
            self.m_IndexPath = Some(path.to_string());
        }
        let (header, blockTable, vectorIndex, docData) = {
            let store = self.m_Store.lock().unwrap();
            Self::BuildIndexData(&store, false)
        };

        self.m_WriteIndexFileHeader = header;
        self.m_WriteBlockTable = blockTable;
        self.m_WriteVectorIndex = vectorIndex;
        self.m_WriteDocData = docData;

        IndexSerializer::Save(&self.m_WriteIndexFileHeader, &self.m_WriteBlockTable, &self.m_WriteDocData, path)?;
        if savingDeltaIndex {
            let mut delta = IndexContext::with_path_and_load_delta(Some(path.to_string()), false);
            if delta.m_Built && delta.DocumentCount() > 0 {
                delta.m_LoadedFromDisk = true;
                self.m_DeltaContext = Some(Box::new(delta));
            }
        }
        Ok(())
    }

    #[allow(non_snake_case)]
    pub fn Merge(&mut self, output_path: &str) -> Result<()> {
        let Some(basePath) = self.m_IndexPath.clone() else { return Err(RustBladeError::IndexNotBuilt); };
        if !self.m_Built || self.m_DocData.is_empty() { return Err(RustBladeError::IndexNotBuilt); }

        let deltaPath = Self::DeltaIndexPath(&basePath);
        if !IndexSerializer::is_valid_index(&deltaPath) { return Err(RustBladeError::InvalidFormat); }

        let delta = IndexContext::with_path_and_load_delta(Some(deltaPath), false);
        if !delta.m_Built || delta.m_DocData.is_empty() { return Err(RustBladeError::IndexNotBuilt); }

        let tempPath = format!("{output_path}.tmp");
        let prefix = output_path.to_string();
        let headTempPath = format!("{prefix}.head.tmp");
        let leafTempPath = format!("{prefix}.leaf.tmp");
        let indexTempPath = format!("{prefix}.blocks.tmp");
        let docDataTempPath = format!("{prefix}.docdata.tmp");
        Self::CleanupTempFiles(&[&tempPath, &headTempPath, &leafTempPath, &indexTempPath, &docDataTempPath]);

        let baseDocCount = self.m_IndexFileHeader.IFH_NumDocuments as usize;
        let deltaFirstDocId = baseDocCount;
        let deltaDocLimit = delta.m_IndexFileHeader.IFH_NumDocuments as usize;
        if deltaDocLimit < deltaFirstDocId { return Err(RustBladeError::InvalidFormat); }

        let deltaDocCount = deltaDocLimit - deltaFirstDocId;
        let mergedDocs = deltaFirstDocId + deltaDocCount;

        let mut mergedDocData = Vec::with_capacity(mergedDocs * DOC_REC_SIZE);
        let baseDocBytes = baseDocCount * DOC_REC_SIZE;
        if baseDocBytes > self.m_DocData.len() { return Err(RustBladeError::InvalidFormat); }
        mergedDocData.extend_from_slice(&self.m_DocData[..baseDocBytes]);
        if deltaDocCount > 0 {
            let begin = deltaFirstDocId * DOC_REC_SIZE;
            let end = deltaDocLimit * DOC_REC_SIZE;
            let deltaSlice = delta.m_DocData.get(begin..end).ok_or(RustBladeError::InvalidFormat)?;
            mergedDocData.extend_from_slice(deltaSlice);
        }
        fs::write(&docDataTempPath, &mergedDocData)?;

        let mut headEntryCount = 0u64;
        let mut totalTerms = 0u64;
        let mut indexBlockCount = 0u32;
        {
            let mut headFile = File::create(&headTempPath)?;
            let mut indexFile = File::create(&indexTempPath)?;
            let mut leafFile = File::create(&leafTempPath)?;

            let mut baseCursor = LeafTermBlockView::new(&self.m_BlockTable);
            let mut deltaCursor = LeafTermBlockView::new(&delta.m_BlockTable);
            const MERGED_LEAF_BLOCK_LIMIT: u32 = 32 * 1024;
            const MERGED_INDEX_BLOCK_LIMIT: u32 = 32 * 1024;
            let mut mergedCursor = LeafTermBlockView::new_write(MERGED_LEAF_BLOCK_LIMIT, MERGED_INDEX_BLOCK_LIMIT);

            while baseCursor.Current().is_some() || deltaCursor.Current().is_some() {
                let baseCurrent = baseCursor.Current().cloned();
                let deltaCurrent = deltaCursor.Current().cloned();
                let takeBase = baseCurrent.is_some()
                    && (deltaCurrent.is_none() || !deltaCursor.LessThan(&baseCursor));
                let takeDelta = deltaCurrent.is_some()
                    && (baseCurrent.is_none() || !baseCursor.LessThan(&deltaCursor));
                let mut advanceBase = false;
                let mut advanceDelta = false;

                if takeBase && takeDelta {
                    if mergedCursor.CanAddLeafTermEntry(&baseCursor)
                        && mergedCursor.AddIndexPair(&baseCursor, &deltaCursor)
                        && mergedCursor.AddLeafTermEntry(&baseCursor)
                    {
                        advanceBase = true;
                        advanceDelta = true;
                    } else {
                        let before = (headEntryCount, indexBlockCount);
                        Self::DumpBatch(&mut mergedCursor, &mut headFile, &mut indexFile, &mut leafFile,
                            &mut headEntryCount, &mut totalTerms, &mut indexBlockCount)?;
                        if before == (headEntryCount, indexBlockCount) { return Err(RustBladeError::InvalidFormat); }
                    }
                } else if takeBase {
                    if mergedCursor.CanAddLeafTermEntry(&baseCursor)
                        && mergedCursor.AddIndex(&baseCursor)
                        && mergedCursor.AddLeafTermEntry(&baseCursor)
                    {
                        advanceBase = true;
                    } else {
                        let before = (headEntryCount, indexBlockCount);
                        Self::DumpBatch(&mut mergedCursor, &mut headFile, &mut indexFile, &mut leafFile,
                            &mut headEntryCount, &mut totalTerms, &mut indexBlockCount)?;
                        if before == (headEntryCount, indexBlockCount) { return Err(RustBladeError::InvalidFormat); }
                    }
                } else if takeDelta {
                    if mergedCursor.CanAddLeafTermEntry(&deltaCursor)
                        && mergedCursor.AddIndex(&deltaCursor)
                        && mergedCursor.AddLeafTermEntry(&deltaCursor)
                    {
                        advanceDelta = true;
                    } else {
                        let before = (headEntryCount, indexBlockCount);
                        Self::DumpBatch(&mut mergedCursor, &mut headFile, &mut indexFile, &mut leafFile,
                            &mut headEntryCount, &mut totalTerms, &mut indexBlockCount)?;
                        if before == (headEntryCount, indexBlockCount) { return Err(RustBladeError::InvalidFormat); }
                    }
                } else {
                    return Err(RustBladeError::InvalidFormat);
                }

                if advanceBase { baseCursor.Advance(); }
                if advanceDelta { deltaCursor.Advance(); }
            }

            Self::DumpBatch(&mut mergedCursor, &mut headFile, &mut indexFile, &mut leafFile,
                &mut headEntryCount, &mut totalTerms, &mut indexBlockCount)?;
        }

        let headOffset = INDEX_FILE_HEADER_SIZE;
        let leafOffset = headOffset + headEntryCount as usize * 32;
        let docDataOffset = leafOffset + headEntryCount as usize * PAGE_SIZE;
        let indexOffset = docDataOffset + mergedDocData.len();
        let totalDocLength = (self.m_IndexFileHeader.IFH_AvgDocLength as f64 * baseDocCount as f64)
            + (delta.m_IndexFileHeader.IFH_AvgDocLength as f64 * deltaDocLimit as f64);
        let avgDocLength = if mergedDocs == 0 { 1.0 } else { (totalDocLength / mergedDocs as f64) as f32 };

        let header = IndexFileHeader {
            IFH_AvgDocLength: avgDocLength,
            IFH_NumDocuments: mergedDocs as u64,
            IFH_NumTerms: totalTerms,
            IFH_HeadTermEntryOffset: headOffset as u64,
            IFH_HeadTermEntryCount: headEntryCount,
            IFH_LeafTermBlockOffset: leafOffset as u64,
            IFH_LeafTermBlockCount: headEntryCount,
            IFH_DocDataOffset: docDataOffset as u64,
            IFH_IndexBlockOffset: indexOffset as u64,
            IFH_IndexBlockCount: indexBlockCount as u64,
        };

        let _ = fs::remove_file(&tempPath);

        {
            let mut output = File::create(&tempPath)?;
            output.write_all(&header.to_bytes())?;
            Self::AppendFile(&mut output, &headTempPath)?;
            Self::AppendFile(&mut output, &leafTempPath)?;
            Self::AppendFile(&mut output, &docDataTempPath)?;
            Self::AppendFile(&mut output, &indexTempPath)?;
            output.flush()?;
        }

        Self::CleanupTempFiles(&[&headTempPath, &leafTempPath, &indexTempPath, &docDataTempPath]);
        let _ = fs::remove_file(output_path);
        fs::rename(&tempPath, output_path)?;

        self.LoadIndex(output_path)
    }

    fn BuildIndexData(store: &PostingStore, buildVectorIndex: bool) -> (IndexFileHeader, IndexBlockTable, HnswIndex, Vec<u8>) {
        let blocks = IndexSerializer::BuildBlocks(store);
        let docData = Self::EncodeDocData(store);
        let documentCount = store.AllDocStats().keys().copied().max().map(|id| id + 1).unwrap_or(0);

        let headOffset = INDEX_FILE_HEADER_SIZE;
        let leafOffset = headOffset + blocks.BBR_HeadTermEntries.len() * 32;
        let docDataOffset = leafOffset + blocks.BBR_LeafTermBlocks.len() * PAGE_SIZE;
        let indexOffset = docDataOffset + docData.len();

        let header = IndexFileHeader {
            IFH_AvgDocLength: if documentCount == 0 { 1.0 } else { store.TotalTerms() as f32 / documentCount as f32 },
            IFH_NumDocuments: documentCount,
            IFH_NumTerms: blocks.BBR_TotalTerms,
            IFH_HeadTermEntryOffset: headOffset as u64,
            IFH_HeadTermEntryCount: blocks.BBR_HeadTermEntries.len() as u64,
            IFH_LeafTermBlockOffset: leafOffset as u64,
            IFH_LeafTermBlockCount: blocks.BBR_LeafTermBlocks.len() as u64,
            IFH_DocDataOffset: docDataOffset as u64,
            IFH_IndexBlockOffset: indexOffset as u64,
            IFH_IndexBlockCount: blocks.BBR_IndexBlocks.len() as u64,
        };

        let mut blockTable = IndexBlockTable::new(blocks.BBR_IndexBlocks.len().max(512) + 64);
        blockTable.SetIndexBlocks(blocks.BBR_IndexBlocks);
        blockTable.SetHeadLeafTermTable(blocks.BBR_HeadTermEntries, blocks.BBR_LeafTermBlocks);

        let mut vectorIndex = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vectorIndex.SetDocData(docData.clone());
        if buildVectorIndex {
            for docId in 0..header.IFH_NumDocuments { vectorIndex.Add(docId); }
        }

        (header, blockTable, vectorIndex, docData)
    }

    fn EnsureVectorIndexBuilt(&mut self) {
        if self.m_VectorBuilt { return; }
        let mut vectorIndex = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vectorIndex.SetDocData(self.m_DocData.clone());
        for docId in 0..self.m_IndexFileHeader.IFH_NumDocuments { vectorIndex.Add(docId); }
        self.m_VectorIndex = vectorIndex;
        self.m_VectorBuilt = true;
    }

    fn EncodeDocData(store: &PostingStore) -> Vec<u8> {
        let documentCount = store.AllDocStats().keys().copied().max().map(|id| id + 1).unwrap_or(0);
        let mut out = vec![0u8; documentCount as usize * DOC_REC_SIZE];
        for docId in 0..documentCount as usize {
            let offset = docId * DOC_REC_SIZE;
            out[offset..offset + 8].copy_from_slice(&u64::MAX.to_le_bytes());
        }
        for (docId, stats) in store.AllDocStats() {
            let offset = *docId as usize * DOC_REC_SIZE;
            out[offset..offset + 8].copy_from_slice(&docId.to_le_bytes());
            out[offset + 32..offset + 36].copy_from_slice(&stats.doc_len.to_le_bytes());
            out[offset + 44..offset + 48].copy_from_slice(&stats.importance.to_le_bytes());
            out[offset + 144..offset + 146].copy_from_slice(&(DOC_VECTOR_DIM as u16).to_le_bytes());
            out[offset + 146..offset + 148].copy_from_slice(&1u16.to_le_bytes());
            let vector = store.GetDocVector(*docId);
            for i in 0..DOC_VECTOR_DIM {
                out[offset + 256 + i] = vector[i] as u8;
            }
            let pathLen = stats.path.len().min(DOC_PATH_MAX);
            out[offset + 72..offset + 74].copy_from_slice(&(pathLen as u16).to_le_bytes());
            out[offset + 768..offset + 768 + pathLen].copy_from_slice(&stats.path.as_bytes()[..pathLen]);
        }
        out
    }

    pub fn LoadIndex(&mut self, path: &str) -> Result<()> {
        self.m_IndexPath = Some(path.to_string());
        let mut store = PostingStore::new();
        let (header, head_term_entries, docdata) = IndexSerializer::load_file_tables(&mut store, path)?;
        let mut table = IndexBlockTable::new(header.IFH_IndexBlockCount as usize);
        table.InitFileBacked(
            path,
            header.IFH_IndexBlockOffset,
            header.IFH_IndexBlockCount as u32,
            header.IFH_IndexBlockCount.min(25_600) as u32,
            header.IFH_LeafTermBlockOffset,
            header.IFH_LeafTermBlockCount as u32,
            header.IFH_LeafTermBlockCount.min(25_600) as u32,
        )?;
        table.SetHeadEntries(head_term_entries);

        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocData(docdata.clone());

        self.m_Store       = Arc::new(Mutex::new(store));
        self.m_BlockTable = Arc::new(table);
        self.m_VectorIndex = vector_index;
        self.m_VectorBuilt = false;
        self.m_IndexFileHeader = header;
        self.m_DocData = docdata;
        self.m_Built       = true;
        self.m_LoadedFromDisk = true;
        self.LoadDeltaIndex();
        Ok(())
    }

    /// Load from raw bytes (WASM path — no file system access needed).
    pub fn LoadFromBytes(&mut self, data: &[u8]) -> Result<()> {
        let mut store = PostingStore::new();
        let (head_term_entries, leaf_term_blocks, blocks, docdata) = IndexSerializer::decode(&mut store, data)?;
        let mut table = IndexBlockTable::new(blocks.len().max(512) + 64);
        table.SetIndexBlocks(blocks);
        table.SetHeadLeafTermTable(head_term_entries, leaf_term_blocks);

        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocData(docdata.clone());

        self.m_Store       = Arc::new(Mutex::new(store));
        self.m_BlockTable = Arc::new(table);
        self.m_VectorIndex = vector_index;
        self.m_VectorBuilt = false;
        self.m_DocData = docdata;
        self.m_Built       = true;
        self.m_LoadedFromDisk = true;
        Ok(())
    }

    fn DeltaIndexPath(path: &str) -> String {
        let slash = path.rfind(['/', '\\']);
        let dot = path.rfind('.');
        if let Some(dot_pos) = dot {
            if slash.map(|slash_pos| dot_pos > slash_pos).unwrap_or(true) {
                return format!("{}{}.{}", &path[..dot_pos], ".delta", &path[dot_pos + 1..]);
            }
        }
        format!("{path}.delta.idx")
    }

    fn LoadDeltaIndex(&mut self) {
        self.m_DeltaContext = None;
        if !self.m_LoadDelta { return; }
        let Some(indexPath) = self.m_IndexPath.clone() else { return; };
        let deltaPath = Self::DeltaIndexPath(&indexPath);
        if deltaPath == indexPath || !IndexSerializer::is_valid_index(&deltaPath) { return; }

        let delta = IndexContext::with_path_and_load_delta(Some(deltaPath), false);
        if delta.m_LoadedFromDisk && delta.DocumentCount() > 0 {
            self.m_DeltaContext = Some(Box::new(delta));
        }
    }

    fn CleanupTempFiles(paths: &[&str]) {
        for path in paths {
            let _ = fs::remove_file(path);
        }
    }

    fn AppendFile(output: &mut File, path: &str) -> Result<()> {
        let mut input = File::open(path)?;
        let mut buffer = [0u8; PAGE_SIZE * 16];
        loop {
            let bytes = input.read(&mut buffer)?;
            if bytes == 0 { return Ok(()); }
            output.write_all(&buffer[..bytes])?;
        }
    }

    fn DumpBatch(mergedCursor: &mut LeafTermBlockView,
                 headFile: &mut File,
                 indexFile: &mut File,
                 leafFile: &mut File,
                 headEntryCount: &mut u64,
                 totalTerms: &mut u64,
                 indexBlockCount: &mut u32) -> Result<()> {
        let usedLeafBlocks = mergedCursor.UsedLeafBlockCount();
        let usedIndexBlocks = mergedCursor.UsedIndexBlockCount();
        if usedLeafBlocks == 0 && usedIndexBlocks == 0 { return Ok(()); }

        for leafBlockID in 0..usedLeafBlocks as usize {
            let leaf = &mergedCursor.m_LeafBlocks[leafBlockID];
            let entryCount = leaf.entry_count().min(LEAF_TERM_DIRECTORY_COUNT - 1);
            let firstEntry = leaf.entry(0).ok_or(RustBladeError::InvalidFormat)?;
            let head = HeadTermEntry::new(&firstEntry.LTE_Term, *headEntryCount as u32 + leafBlockID as u32);
            headFile.write_all(&head.to_bytes())?;
            leafFile.write_all(&leaf.to_bytes())?;
            *totalTerms += entryCount as u64;
        }

        for indexBlockID in 0..usedIndexBlocks as usize {
            indexFile.write_all(&mergedCursor.m_IndexBlocks[indexBlockID].IB_Data)?;
        }

        *headEntryCount += usedLeafBlocks as u64;
        *indexBlockCount += usedIndexBlocks;
        mergedCursor.ResetWrite(*indexBlockCount);
        Ok(())
    }

    // ── ISR tree builder ─────────────────────────────────────────────────────

    fn BuildIndexReader(&self, node: Option<EvalNode>) -> Box<dyn IndexReader> {
        match node {
            None => Box::new(AdvancedIndexReader::open("", Arc::clone(&self.m_BlockTable), 0)),

            Some(EvalNode::Term(tn)) => {
                let docFreq = self.m_Store.lock().unwrap().DocFreq(&tn.stream_key);
                Box::new(AdvancedIndexReader::open(&tn.stream_key, Arc::clone(&self.m_BlockTable), docFreq))
            }

            Some(EvalNode::And(an)) => {
                let children: Vec<Box<dyn IndexReader>> = an.children.into_iter()
                    .map(|c| self.BuildIndexReader(Some(c))).collect();
                if children.iter().any(|c| c.IsEnd()) {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.m_BlockTable), 0));
                }
                let mut v = children;
                if v.len() == 1 { return v.remove(0); }
                Box::new(AndIndexReader::new(v))
            }

            Some(EvalNode::Or(on)) => {
                let children: Vec<Box<dyn IndexReader>> = on.children.into_iter()
                    .map(|c| self.BuildIndexReader(Some(c)))
                    .filter(|r| !r.IsEnd()).collect();
                if children.is_empty() {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.m_BlockTable), 0));
                }
                let mut v = children;
                if v.len() == 1 { return v.remove(0); }
                Box::new(OrIndexReader::new(v))
            }

            Some(EvalNode::Not(nn)) => {
                let base    = self.BuildIndexReader(Some(*nn.base));
                let exclude = self.BuildIndexReader(Some(*nn.exclude));
                Box::new(NotIndexReader::new(base, exclude))
            }
        }
    }

    fn BuildVectorIndexReader(&mut self, query: &[f32], efSearch: usize) -> Box<dyn IndexReader> {
        let results = self.VectorSearch(query, 0, efSearch);
        Box::new(VectorIndexReader::new(results))
    }
}

#[allow(non_snake_case)]
struct LeafTermBlockView<'a> {
    m_BlockTable: Option<&'a IndexBlockTable>,
    m_LeafTermBlockCount: u32,
    m_IndexBlockCount: u32,
    m_LeafBlockID: u32,
    m_EntryIndex: usize,
    m_CurrentLeaf: Option<PinnedBlock<LeafTermBlock>>,
    m_CurrentEntry: Option<LeafTermEntry>,
    m_LeafBlocks: Vec<LeafTermBlock>,
    m_IndexBlocks: Vec<IndexBlock>,
    m_PostingIndexBlockID: u32,
    m_PostingIndexOffset: usize,
    m_PostingIndexLength: usize,
    m_PostingContinuationBlockCount: u32,
    m_DocFreq: u32,
    m_LeafWriteOffset: usize,
    m_LeafEntryCount: usize,
    m_IndexBlockID: u32,
    m_IndexOffsetInBlock: usize,
    m_IndexBlockBase: u32,
    m_HasIndexWrite: bool,
}

const POSTING_SCRATCH_PAGE_COUNT: usize = 100;
const POSTING_SCRATCH_BYTES: usize = POSTING_SCRATCH_PAGE_COUNT * PAGE_SIZE;

#[allow(non_snake_case)]
fn WriteLeafEntry(block: &mut LeafTermBlock, entryIndex: usize, offset: usize, entry: &LeafTermEntry) {
    block.LTB_Directory[entryIndex] = (LEAF_TERM_DATA_OFFSET + offset) as u16;
    let data = &mut block.LTB_Data[offset..];
    data[0..4].copy_from_slice(&entry.LTE_DocFreq.to_le_bytes());
    data[4..8].copy_from_slice(&entry.LTE_IndexBlockID.to_le_bytes());
    data[8..12].copy_from_slice(&entry.LTE_IndexOffset.to_le_bytes());
    data[12..16].copy_from_slice(&entry.LTE_IndexLength.to_le_bytes());
    data[16..20].copy_from_slice(&entry.LTE_ContinuationBlockCount.to_le_bytes());
    data[20..24].copy_from_slice(&entry.LTE_Flags.to_le_bytes());
    data[24] = entry.LTE_Term.len() as u8;
    data[25..25 + entry.LTE_Term.len()].copy_from_slice(entry.LTE_Term.as_bytes());
}

#[allow(non_snake_case)]
fn ReadVbPairEnd(data: &[u8], offset: &mut usize) -> bool {
    let readOne = |data: &[u8], offset: &mut usize| -> bool {
        while *offset < data.len() {
            let byte = data[*offset];
            *offset += 1;
            if byte & 0x80 == 0 { return true; }
        }
        false
    };
    readOne(data, offset) && readOne(data, offset)
}

#[allow(non_snake_case)]
fn PostingSplitLength(data: &[u8], capacity: usize) -> usize {
    if data.len() <= capacity { return data.len(); }

    let mut cursor = 0usize;
    let mut lastPairEnd = 0usize;
    let limit = data.len().min(capacity);
    while cursor < limit {
        if !ReadVbPairEnd(data, &mut cursor) || cursor > limit {
            break;
        }
        lastPairEnd = cursor;
    }
    lastPairEnd
}

#[allow(non_snake_case)]
fn MaxDocIDInPairs(data: &[u8]) -> u64 {
    let mut cursor = 0usize;
    let mut maxDocID = 0u64;
    while cursor < data.len() {
        let (docID, docBytes) = VbReadLocal(data, cursor);
        cursor += docBytes;
        if cursor >= data.len() { break; }
        let (_tf, tfBytes) = VbReadLocal(data, cursor);
        cursor += tfBytes;
        maxDocID = docID;
    }
    maxDocID
}

#[allow(non_snake_case)]
fn VbReadLocal(data: &[u8], start: usize) -> (u64, usize) {
    let mut value = 0u64;
    let mut shift = 0u8;
    let mut pos = start;
    loop {
        if pos >= data.len() { break; }
        let byte = data[pos];
        pos += 1;
        value |= ((byte & 0x7F) as u64) << shift;
        if byte & 0x80 == 0 { break; }
        shift += 7;
    }
    (value, pos - start)
}

#[allow(non_snake_case)]
impl<'a> LeafTermBlockView<'a> {
    fn new(block_table: &'a IndexBlockTable) -> Self {
        let mut view = Self {
            m_BlockTable: Some(block_table),
            m_LeafTermBlockCount: block_table.LeafTermBlockCount(),
            m_IndexBlockCount: 0,
            m_LeafBlockID: 0,
            m_EntryIndex: 0,
            m_CurrentLeaf: None,
            m_CurrentEntry: None,
            m_LeafBlocks: Vec::new(),
            m_IndexBlocks: Vec::new(),
            m_PostingIndexBlockID: 0,
            m_PostingIndexOffset: 0,
            m_PostingIndexLength: 0,
            m_PostingContinuationBlockCount: 0,
            m_DocFreq: 0,
            m_LeafWriteOffset: 0,
            m_LeafEntryCount: 0,
            m_IndexBlockID: 0,
            m_IndexOffsetInBlock: 0,
            m_IndexBlockBase: 0,
            m_HasIndexWrite: false,
        };
        view.GoCurrent();
        view
    }

    fn new_write(leaf_term_block_count: u32, index_block_count: u32) -> Self {
        Self {
            m_BlockTable: None,
            m_LeafTermBlockCount: leaf_term_block_count,
            m_IndexBlockCount: index_block_count,
            m_LeafBlockID: 0,
            m_EntryIndex: 0,
            m_CurrentLeaf: None,
            m_CurrentEntry: None,
            m_LeafBlocks: vec![LeafTermBlock::default(); leaf_term_block_count as usize],
            m_IndexBlocks: vec![IndexBlock::default(); index_block_count as usize],
            m_PostingIndexBlockID: 0,
            m_PostingIndexOffset: 0,
            m_PostingIndexLength: 0,
            m_PostingContinuationBlockCount: 0,
            m_DocFreq: 0,
            m_LeafWriteOffset: 0,
            m_LeafEntryCount: 0,
            m_IndexBlockID: 0,
            m_IndexOffsetInBlock: 0,
            m_IndexBlockBase: 0,
            m_HasIndexWrite: false,
        }
    }

    fn Current(&self) -> Option<&LeafTermEntry> {
        self.m_CurrentEntry.as_ref()
    }

    fn Advance(&mut self) {
        if self.m_CurrentEntry.is_none() { return; }
        self.m_EntryIndex += 1;
        self.GoCurrent();
    }

    fn LessThan(&self, other: &Self) -> bool {
        self.Current().map(|entry| entry.LTE_Term.as_str()).unwrap_or("")
            < other.Current().map(|entry| entry.LTE_Term.as_str()).unwrap_or("")
    }

    fn CanAddLeafTermEntry(&self, cursor: &Self) -> bool {
        let Some(source) = cursor.Current() else { return false; };
        if source.LTE_Term.len() > HEAD_TERM_KEY_MAX { return true; }

        let entry_bytes = source.byte_len();
        if entry_bytes > PAGE_SIZE - LEAF_TERM_DATA_OFFSET { return false; }

        let mut leafBlockID = self.m_LeafBlockID;
        if self.m_LeafEntryCount > 0
            && (self.m_LeafEntryCount >= LEAF_TERM_DIRECTORY_COUNT - 1
                || self.m_LeafWriteOffset + entry_bytes > PAGE_SIZE - LEAF_TERM_DATA_OFFSET)
        {
            leafBlockID += 1;
        }
        leafBlockID < self.m_LeafTermBlockCount
    }

    fn AddLeafTermEntry(&mut self, cursor: &Self) -> bool {
        let Some(source) = cursor.Current() else { return false; };
        if source.LTE_Term.len() > HEAD_TERM_KEY_MAX { return true; }

        let entry_bytes = source.byte_len();
        if self.m_LeafEntryCount > 0
            && (self.m_LeafEntryCount >= LEAF_TERM_DIRECTORY_COUNT - 1
                || self.m_LeafWriteOffset + entry_bytes > PAGE_SIZE - LEAF_TERM_DATA_OFFSET)
        {
            self.m_LeafBlocks[self.m_LeafBlockID as usize].LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = self.m_LeafEntryCount as u16;
            self.m_LeafBlockID += 1;
            self.m_LeafWriteOffset = 0;
            self.m_LeafEntryCount = 0;
        }

        if self.m_LeafBlockID >= self.m_LeafTermBlockCount || entry_bytes > PAGE_SIZE - LEAF_TERM_DATA_OFFSET {
            return false;
        }

        let entry = LeafTermEntry {
            LTE_Term: source.LTE_Term.clone(),
            LTE_DocFreq: self.m_DocFreq,
            LTE_IndexBlockID: self.m_IndexBlockBase + self.m_PostingIndexBlockID,
            LTE_IndexOffset: self.m_PostingIndexOffset as u32,
            LTE_IndexLength: self.m_PostingIndexLength as u32,
            LTE_ContinuationBlockCount: self.m_PostingContinuationBlockCount,
            LTE_Flags: source.LTE_Flags,
        };

        WriteLeafEntry(&mut self.m_LeafBlocks[self.m_LeafBlockID as usize], self.m_LeafEntryCount, self.m_LeafWriteOffset, &entry);
        self.m_LeafWriteOffset += entry_bytes;
        self.m_LeafEntryCount += 1;
        self.m_LeafBlocks[self.m_LeafBlockID as usize].LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = self.m_LeafEntryCount as u16;
        self.m_PostingIndexLength = 0;
        self.m_PostingContinuationBlockCount = 0;
        true
    }

    fn AddIndex(&mut self, cursor: &Self) -> bool {
        let Some(source) = cursor.Current() else { return false; };
        let Some(bytes) = cursor.PostingBytes() else { return false; };
        if bytes.len() > POSTING_SCRATCH_BYTES { return false; }
        if !self.AddIndexBytes(&bytes) { return false; }
        self.m_DocFreq = source.LTE_DocFreq;
        true
    }

    fn AddIndexPair(&mut self, smallCursor: &Self, bigCursor: &Self) -> bool {
        let (Some(small), Some(big)) = (smallCursor.Current(), bigCursor.Current()) else { return false; };
        let Some(mut bytes) = smallCursor.PostingBytes() else { return false; };
        let Some(big_bytes) = bigCursor.PostingBytes() else { return false; };
        if bytes.len() > POSTING_SCRATCH_BYTES || big_bytes.len() > POSTING_SCRATCH_BYTES - bytes.len() { return false; }
        bytes.extend_from_slice(&big_bytes);
        if !self.AddIndexBytes(&bytes) { return false; }
        self.m_DocFreq = small.LTE_DocFreq + big.LTE_DocFreq;
        true
    }

    fn UsedLeafBlockCount(&self) -> u32 {
        if self.m_LeafEntryCount > 0 { self.m_LeafBlockID + 1 } else { self.m_LeafBlockID }
    }

    fn UsedIndexBlockCount(&self) -> u32 {
        if self.m_HasIndexWrite { self.m_IndexBlockID + 1 } else { 0 }
    }

    fn ResetWrite(&mut self, indexBlockBase: u32) {
        self.m_PostingIndexBlockID = 0;
        self.m_PostingIndexOffset = 0;
        self.m_PostingIndexLength = 0;
        self.m_PostingContinuationBlockCount = 0;
        self.m_DocFreq = 0;
        self.m_LeafBlockID = 0;
        self.m_LeafWriteOffset = 0;
        self.m_LeafEntryCount = 0;
        self.m_IndexBlockID = 0;
        self.m_IndexOffsetInBlock = 0;
        self.m_IndexBlockBase = indexBlockBase;
        self.m_HasIndexWrite = false;
        for block in &mut self.m_LeafBlocks { *block = LeafTermBlock::default(); }
        for block in &mut self.m_IndexBlocks { *block = IndexBlock::default(); }
    }

    fn PostingBytes(&self) -> Option<Vec<u8>> {
        let blockTable = self.m_BlockTable?;
        let entry = self.Current()?;
        blockTable.PostingBytes(entry)
    }

    fn AddIndexBytes(&mut self, sourceBytes: &[u8]) -> bool {
        const DATA_CAP: usize = PAGE_SIZE;
        const CONT_HDR: usize = INDEX_BLOCK_CONTINUATION_HEADER_SIZE;

        let mut targetBlockID = self.m_IndexBlockID;
        let mut targetOffset = self.m_IndexOffsetInBlock;
        let mut splitLength = PostingSplitLength(sourceBytes, DATA_CAP.saturating_sub(targetOffset));
        if splitLength == 0 {
            targetBlockID += 1;
            targetOffset = 0;
            splitLength = PostingSplitLength(sourceBytes, DATA_CAP);
            if splitLength == 0 { return false; }
        }
        if targetBlockID >= self.m_IndexBlockCount { return false; }

        let mut continuations = Vec::new();
        let mut sourceOffset = splitLength;
        while sourceOffset < sourceBytes.len() {
            let continuationLength = PostingSplitLength(&sourceBytes[sourceOffset..], DATA_CAP - CONT_HDR);
            if continuationLength == 0 { return false; }
            continuations.push((sourceOffset, continuationLength, MaxDocIDInPairs(&sourceBytes[sourceOffset..sourceOffset + continuationLength])));
            sourceOffset += continuationLength;
        }

        let blocksNeeded = 1usize + continuations.len();
        if blocksNeeded > self.m_IndexBlockCount as usize - targetBlockID as usize { return false; }

        self.m_IndexBlocks[targetBlockID as usize].IB_Data[targetOffset..targetOffset + splitLength]
            .copy_from_slice(&sourceBytes[..splitLength]);

        for (i, (offset, length, maxDocID)) in continuations.iter().copied().enumerate() {
            let continuationBlockID = targetBlockID + 1 + i as u32;
            IndexBlockContinuationHeader {
                IBCH_MaxDocID: maxDocID,
                IBCH_DataLength: length as u32,
            }.write_to(&mut self.m_IndexBlocks[continuationBlockID as usize].IB_Data[..CONT_HDR]);
            self.m_IndexBlocks[continuationBlockID as usize].IB_Data[CONT_HDR..CONT_HDR + length]
                .copy_from_slice(&sourceBytes[offset..offset + length]);
        }

        self.m_IndexBlockID = targetBlockID + continuations.len() as u32;
        self.m_IndexOffsetInBlock = if continuations.is_empty() { targetOffset + splitLength } else { DATA_CAP };
        self.m_PostingIndexBlockID = targetBlockID;
        self.m_PostingIndexOffset = targetOffset;
        self.m_PostingIndexLength = splitLength;
        self.m_PostingContinuationBlockCount = continuations.len() as u32;
        self.m_HasIndexWrite = true;
        true
    }

    fn GoCurrent(&mut self) {
        self.m_CurrentEntry = None;

        while self.m_LeafBlockID < self.m_LeafTermBlockCount {
            if self.m_CurrentLeaf.is_none() {
                self.m_CurrentLeaf = self.m_BlockTable.unwrap().GetLeafBlockBySeq(self.m_LeafBlockID);
                if self.m_CurrentLeaf.is_none() { return; }
            }

            let leaf = self.m_CurrentLeaf.as_ref().unwrap();
            if self.m_EntryIndex < leaf.entry_count() {
                self.m_CurrentEntry = leaf.entry(self.m_EntryIndex);
                return;
            }

            self.m_CurrentLeaf = None;
            self.m_LeafBlockID += 1;
            self.m_EntryIndex = 0;
        }
    }
}


impl Default for IndexContext {
    fn default() -> Self { Self::new() }
}
