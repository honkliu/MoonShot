use std::fs;
use std::fs::File;
use std::collections::VecDeque;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::{mpsc, Arc, Condvar, Mutex, RwLock};
use std::thread::{self, JoinHandle};
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
use crate::block_table::{DOC_PATH_MAX, DOC_PATH_PREFIX_ID_BYTES, DOC_PATH_FILENAME_MAX, DOC_PATH_PREFIX_INVALID, DOC_REC_SIZE, DOC_VECTOR_DIM, DOC_VECTOR_OFFSET, DOC_PATH_OFFSET, PATH_PREFIX_SIDECAR_BYTES, INDEX_FILE_HEADER_SIZE, PAGE_SIZE, HEAD_TERM_KEY_MAX, INDEX_BLOCK_CONTINUATION_HEADER_SIZE, LEAF_TERM_DATA_OFFSET, LEAF_TERM_DIRECTORY_COUNT, TERM_MPHF_HEADER_SIZE, HeadTermEntry, IndexBlock, IndexBlockContinuationHeader, LeafTermEntry, LeafTermBlock, PinnedBlock, TermMphfHeader, DocDataEncodeScore};
use crate::vector_index::{HnswIndex, VectorMetric, VectorSearchResult};
use crate::vector_index::build_hashed_embedding;
use crate::executor::{IndexSearchExecutor, SearchResult};

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

struct SearchTaskState {
    query: String,
    vector: Vec<f32>,
    streams: String,
    top_k: usize,
    reply: mpsc::Sender<Vec<SearchResult>>,
}

struct SearchQueueState {
    queue: VecDeque<SearchTaskState>,
    stop: bool,
}

struct SearchRuntime {
    store: Arc<RwLock<PostingStore>>,
    block_table: Arc<IndexBlockTable>,
    vector_index: Arc<HnswIndex>,
    docdata: Arc<Vec<u8>>,
    header: IndexFileHeader,
    delta: Option<Arc<SearchRuntime>>,
}

impl SearchRuntime {
    fn from_context(ctx: &IndexContext) -> Arc<Self> {
        Arc::new(Self {
            store: Arc::clone(&ctx.m_Store),
            block_table: Arc::clone(&ctx.m_BlockTable),
            vector_index: Arc::new(ctx.m_VectorIndex.clone()),
            docdata: Arc::new(ctx.m_DocData.clone()),
            header: ctx.m_IndexFileHeader,
            delta: ctx.m_DeltaContext.as_ref().map(|delta| Self::from_context(delta)),
        })
    }

    fn get_reader(&self, tree: EvalTree) -> Box<dyn IndexReader> {
        let base = self.build_local_reader(tree.clone());
        if let Some(delta) = self.delta.as_ref() {
            return Box::new(OrIndexReader::new(vec![base, delta.build_local_reader(tree)]));
        }
        base
    }

    fn build_local_reader(&self, tree: EvalTree) -> Box<dyn IndexReader> {
        if tree.HasTextQuery() && tree.HasVectorQuery() {
            return self.build_index_reader(tree.root);
        }
        if tree.HasVectorQuery() {
            return Box::new(VectorIndexReader::new(self.vector_index.Search(&tree.vector_query, 0, tree.vector_ef_search)));
        }
        self.build_index_reader(tree.root)
    }

    fn build_index_reader(&self, node: Option<EvalNode>) -> Box<dyn IndexReader> {
        match node {
            None => Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0)),
            Some(EvalNode::Term(tn)) => {
                let doc_freq = self.store.read().unwrap().DocFreq(&tn.stream_key);
                Box::new(AdvancedIndexReader::open(&tn.stream_key, Arc::clone(&self.block_table), doc_freq))
            }
            Some(EvalNode::And(an)) => {
                let children: Vec<Box<dyn IndexReader>> = an.children.into_iter()
                    .map(|c| self.build_index_reader(Some(c))).collect();
                if children.iter().any(|c| c.IsEnd()) {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0));
                }
                let mut children = children;
                if children.len() == 1 { return children.remove(0); }
                Box::new(AndIndexReader::new(children))
            }
            Some(EvalNode::Or(on)) => {
                let children: Vec<Box<dyn IndexReader>> = on.children.into_iter()
                    .map(|c| self.build_index_reader(Some(c)))
                    .filter(|r| !r.IsEnd()).collect();
                if children.is_empty() {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0));
                }
                let mut children = children;
                if children.len() == 1 { return children.remove(0); }
                Box::new(OrIndexReader::new(children))
            }
            Some(EvalNode::Not(nn)) => {
                let base = self.build_index_reader(Some(*nn.base));
                let exclude = self.build_index_reader(Some(*nn.exclude));
                Box::new(NotIndexReader::new(base, exclude))
            }
        }
    }
}

pub struct SearchTask {
    receiver: mpsc::Receiver<Vec<SearchResult>>,
}

#[allow(non_snake_case)]
impl SearchTask {
    pub fn Wait(self) -> Vec<SearchResult> {
        self.receiver.recv().unwrap_or_default()
    }
}

/// Central factory — owns PostingStore, BlockTable and all search components.
/// Mirrors MoonShot's IndexContext.h.
#[allow(non_snake_case)]
pub struct IndexContext {
    m_Store:       Arc<RwLock<PostingStore>>,
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
    m_PathPrefixSidecar: Vec<u8>,
    m_PathPrefixes: Vec<String>,
    m_DeltaContext: Option<Box<IndexContext>>,
    m_WriteBlockTable: IndexBlockTable,
    m_WriteVectorIndex: HnswIndex,
    m_WriteIndexFileHeader: IndexFileHeader,
    m_WriteDocData: Vec<u8>,
    m_WritePathPrefixSidecar: Vec<u8>,
    m_WritePathPrefixes: Vec<String>,
    m_SearchQueue: Arc<(Mutex<SearchQueueState>, Condvar)>,
    m_SearchWorkers: Vec<JoinHandle<()>>,
    m_SearchRuntime: Option<Arc<SearchRuntime>>,
}

#[allow(non_snake_case)]
impl IndexContext {
    pub fn new() -> Self { Self::with_path(None) }

    pub fn with_path(index_path: Option<String>) -> Self {
        Self::with_path_and_load_delta(index_path, true)
    }

    pub fn with_path_and_load_delta(index_path: Option<String>, load_delta: bool) -> Self {
        let store = Arc::new(RwLock::new(PostingStore::new()));
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
            m_PathPrefixSidecar: vec![0u8; PATH_PREFIX_SIDECAR_BYTES],
            m_PathPrefixes: Vec::new(),
            m_DeltaContext: None,
            m_WriteBlockTable: IndexBlockTable::new(512),
            m_WriteVectorIndex: HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine),
            m_WriteIndexFileHeader: IndexFileHeader::default(),
            m_WriteDocData: Vec::new(),
            m_WritePathPrefixSidecar: vec![0u8; PATH_PREFIX_SIDECAR_BYTES],
            m_WritePathPrefixes: Vec::new(),
            m_SearchQueue: Arc::new((Mutex::new(SearchQueueState { queue: VecDeque::new(), stop: false }), Condvar::new())),
            m_SearchWorkers: Vec::new(),
            m_SearchRuntime: None,
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
        let mut nextId = Self::DocDataFirstDocId(&self.m_DocData, &self.m_IndexFileHeader) + self.m_IndexFileHeader.IFH_NumDocuments;
        if let Some(delta) = self.m_DeltaContext.as_ref() {
            nextId = nextId.max(Self::DocDataFirstDocId(&delta.m_DocData, &delta.m_IndexFileHeader) + delta.m_IndexFileHeader.IFH_NumDocuments);
        }
        for docId in self.m_Store.read().unwrap().AllDocStats().keys().copied() {
            nextId = nextId.max(docId + 1);
        }
        nextId
    }

    pub fn AddDocument(&mut self, doc: &Document, buildVector: bool) -> u64 {
        self.StopSearchWorkers();
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
    pub fn GetStore(&self) -> Arc<RwLock<PostingStore>> { Arc::clone(&self.m_Store) }
    pub fn DocumentCount(&self) -> u64 { self.m_IndexFileHeader.IFH_NumDocuments }
    pub fn AvgDocLen(&self) -> f32 { self.m_IndexFileHeader.IFH_AvgDocLength }
    pub fn GetDocPath(&self, doc_id: u64) -> String {
        let docId = ReaderDocumentIDValue(doc_id);
        let path = self.m_Store.read().unwrap().GetDocPath(docId).to_string();
        if !path.is_empty() { return path; }
        let first_doc_id = Self::DocDataFirstDocId(&self.m_DocData, &self.m_IndexFileHeader);
        if docId >= first_doc_id {
            let slot = (docId - first_doc_id) as usize;
            let offset = slot * DOC_REC_SIZE;
            if offset + DOC_REC_SIZE <= self.m_DocData.len()
                && u32::from_le_bytes(self.m_DocData[offset..offset + 4].try_into().unwrap()) as u64 == docId
            {
                let path_len = u16::from_le_bytes(self.m_DocData[offset + 18..offset + 20].try_into().unwrap()) as usize;
                if path_len > 0 && path_len <= DOC_PATH_MAX {
                    return Self::DecodeDocPath(&self.m_DocData[offset + DOC_PATH_OFFSET..offset + DOC_PATH_OFFSET + path_len], &self.m_PathPrefixes);
                }
            }
        }
        self.m_DeltaContext.as_ref().map(|delta| delta.GetDocPath(docId)).unwrap_or_default()
    }

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
        self.StopSearchWorkers();
        if self.m_Built {
            self.BuildVectorRuntime();
            return;
        }

        let store = self.m_Store.read().unwrap();

        let table = Arc::get_mut(&mut self.m_BlockTable)
            .expect("BlockTable must have no other refs during build");

        let (header, newTable, vectorIndex, docData, pathPrefixSidecar, pathPrefixes) = Self::BuildIndexData(&store, true);
        *table = newTable;
        self.m_VectorIndex = vectorIndex;
        self.m_VectorBuilt = true;
        self.m_IndexFileHeader = header;
        self.m_DocData = docData;
        self.m_PathPrefixSidecar = pathPrefixSidecar;
        self.m_PathPrefixes = pathPrefixes;
        self.m_Built = true;
    }

    // ── Search ───────────────────────────────────────────────────────────────

    pub fn Compile(&self, query: &str, stream_set: &str) -> EvalTree {
        self.m_Compiler.Compile(query, stream_set)
    }

    pub fn GetReaderForQuery(&self, query: &str, stream_set: &str)
        -> Box<dyn IndexReader>
    {
        let tree = self.Compile(query, stream_set);
        self.GetReader(tree)
    }

    pub fn GetReader(&self, tree: EvalTree) -> Box<dyn IndexReader> {
        let baseReader = self.BuildLocalReader(tree.clone());
        if let Some(delta) = self.m_DeltaContext.as_ref() {
            return Box::new(OrIndexReader::new(vec![baseReader, delta.BuildLocalReader(tree)]));
        }
        baseReader
    }

    fn BuildLocalReader(&self, tree: EvalTree) -> Box<dyn IndexReader> {
        if tree.HasTextQuery() && tree.HasVectorQuery() {
            return self.BuildIndexReader(tree.root);
        }
        if tree.HasVectorQuery() {
            return self.BuildVectorIndexReader(&tree.vector_query, tree.vector_ef_search);
        }
        self.BuildIndexReader(tree.root)
    }

    pub fn GetStreamReader(&self, stream_key: &str) -> Box<dyn IndexReader> {
        let baseReader = self.BuildLocalStreamReader(stream_key);
        if let Some(delta) = self.m_DeltaContext.as_ref() {
            return Box::new(OrIndexReader::new(vec![baseReader, delta.BuildLocalStreamReader(stream_key)]));
        }
        baseReader
    }

    fn BuildLocalStreamReader(&self, stream_key: &str) -> Box<dyn IndexReader> {
        let docFreq = self.m_Store.read().unwrap().DocFreq(stream_key);
        Box::new(AdvancedIndexReader::open(
            stream_key, Arc::clone(&self.m_BlockTable), docFreq))
    }

    pub fn CompileToVector(&self, query: &str) -> Vec<f32> {
        self.m_Compiler.CompileToVector(query)
    }

    pub fn VectorSearch(&mut self, query: &[f32], top_k: usize, ef_search: usize) -> Vec<VectorSearchResult> {
        self.m_VectorIndex.Search(query, top_k, ef_search)
    }

    pub fn VectorCount(&self) -> usize { self.m_VectorIndex.Size() }
    pub fn VectorDimension(&self) -> usize { self.m_VectorIndex.Dimension() }

    pub fn Enqueue(&mut self, query: &str, vector: Vec<f32>, streams: &str, top_k: usize) -> SearchTask {
        self.EnsureSearchWorkersStarted(4);
        let (sender, receiver) = mpsc::channel();
        let state = SearchTaskState {
            query: query.to_string(),
            vector,
            streams: if streams.is_empty() { "AUTB".to_string() } else { streams.to_string() },
            top_k,
            reply: sender,
        };
        let (lock, cv) = &*self.m_SearchQueue;
        lock.lock().unwrap().queue.push_back(state);
        cv.notify_one();
        SearchTask { receiver }
    }

    fn EnsureSearchWorkersStarted(&mut self, worker_count: usize) {
        if !self.m_SearchWorkers.is_empty() { return; }
        self.m_SearchRuntime = Some(SearchRuntime::from_context(self));
        let runtime = Arc::clone(self.m_SearchRuntime.as_ref().unwrap());
        let (lock, _) = &*self.m_SearchQueue;
        lock.lock().unwrap().stop = false;
        for _ in 0..worker_count.max(1) {
            let queue = Arc::clone(&self.m_SearchQueue);
            let runtime = Arc::clone(&runtime);
            self.m_SearchWorkers.push(thread::spawn(move || {
                IndexContext::SearchWorkerLoop(runtime, queue);
            }));
        }
    }

    fn StopSearchWorkers(&mut self) {
        if self.m_SearchWorkers.is_empty() { return; }
        let (lock, cv) = &*self.m_SearchQueue;
        lock.lock().unwrap().stop = true;
        cv.notify_all();
        for worker in self.m_SearchWorkers.drain(..) {
            let _ = worker.join();
        }
        self.m_SearchRuntime = None;
    }

    fn SearchWorkerLoop(runtime: Arc<SearchRuntime>, queue: Arc<(Mutex<SearchQueueState>, Condvar)>) {
        let compiler = IndexSearchCompiler::new(SmartTokenizer::new());
        loop {
            let task = {
                let (lock, cv) = &*queue;
                let mut guard = lock.lock().unwrap();
                while guard.queue.is_empty() && !guard.stop {
                    guard = cv.wait(guard).unwrap();
                }
                if guard.stop && guard.queue.is_empty() { return; }
                guard.queue.pop_front()
            };

            if let Some(task) = task {
                let results = Self::ExecuteSearchTask(&runtime, &compiler, &task);
                let _ = task.reply.send(results);
            }
        }
    }

    fn ExecuteSearchTask(runtime: &SearchRuntime, compiler: &IndexSearchCompiler, task: &SearchTaskState) -> Vec<SearchResult> {
        let mut tree = if task.query.is_empty() && !task.vector.is_empty() {
            EvalTree::new(None)
        } else {
            compiler.Compile(&task.query, &task.streams)
        };
        if !task.vector.is_empty() {
            tree.vector_query = task.vector.clone();
        }
        if tree.IsEmpty() { return Vec::new(); }

        let vector_query = if tree.HasTextQuery() && tree.HasVectorQuery() {
            Some(tree.vector_query.clone())
        } else {
            None
        };
        let mut reader = runtime.get_reader(tree);
        let store = runtime.store.read().unwrap();
        let executor = IndexSearchExecutor::new(&store);
        executor.ExecuteWithVector(
            reader.as_mut(),
            task.top_k,
            runtime.docdata.as_slice(),
            Self::DocDataFirstDocId(runtime.docdata.as_slice(), &runtime.header),
                vector_query.as_deref())
    }

    // ── Persistence ─────────────────────────────────────────────────────────

    pub fn SaveIndex(&mut self, path: &str) -> Result<()> {
        self.StopSearchWorkers();
        let savingDeltaIndex = self.m_IndexPath.as_ref()
            .map(|index_path| path == Self::DeltaIndexPath(index_path))
            .unwrap_or(false);
        if !savingDeltaIndex {
            self.m_IndexPath = Some(path.to_string());
        }
        let (header, blockTable, vectorIndex, docData, pathPrefixSidecar, pathPrefixes) = {
            let store = self.m_Store.read().unwrap();
            Self::BuildIndexData(&store, false)
        };

        self.m_WriteIndexFileHeader = header;
        self.m_WriteBlockTable = blockTable;
        self.m_WriteVectorIndex = vectorIndex;
        self.m_WriteDocData = docData;
        self.m_WritePathPrefixSidecar = pathPrefixSidecar;
        self.m_WritePathPrefixes = pathPrefixes;

        IndexSerializer::Save(&self.m_WriteIndexFileHeader, &self.m_WriteBlockTable, &self.m_WriteDocData, &self.m_WritePathPrefixSidecar, path)?;
        if savingDeltaIndex {
            let mut delta = IndexContext::with_path_and_load_delta(None, false);
            delta.m_IndexPath = Some(path.to_string());
            delta.m_BlockTable = Arc::new(std::mem::replace(&mut self.m_WriteBlockTable, IndexBlockTable::new(512)));
            delta.m_VectorIndex = std::mem::replace(&mut self.m_WriteVectorIndex, HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine));
            delta.m_IndexFileHeader = self.m_WriteIndexFileHeader;
            delta.m_DocData = std::mem::take(&mut self.m_WriteDocData);
            delta.m_PathPrefixSidecar = std::mem::take(&mut self.m_WritePathPrefixSidecar);
            delta.m_PathPrefixes = std::mem::take(&mut self.m_WritePathPrefixes);
            delta.m_Built = true;
            delta.m_LoadedFromDisk = true;
            delta.m_VectorBuilt = false;

            self.m_WriteIndexFileHeader = IndexFileHeader::default();
            self.m_DeltaContext = Some(Box::new(delta));
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
        let deltaFirstDocId = Self::DocDataFirstDocId(&delta.m_DocData, &delta.m_IndexFileHeader) as usize;
        let deltaDocCount = delta.m_IndexFileHeader.IFH_NumDocuments as usize;
        if deltaDocCount > 0 && deltaFirstDocId != baseDocCount { return Err(RustBladeError::InvalidFormat); }
        let mergedDocs = baseDocCount + deltaDocCount;

        let mut mergedDocData = Vec::with_capacity(mergedDocs * DOC_REC_SIZE);
        let baseDocBytes = baseDocCount * DOC_REC_SIZE;
        if baseDocBytes > self.m_DocData.len() { return Err(RustBladeError::InvalidFormat); }
        mergedDocData.extend_from_slice(&self.m_DocData[..baseDocBytes]);
        if deltaDocCount > 0 {
            let deltaDocBytes = deltaDocCount * DOC_REC_SIZE;
            if deltaDocBytes > delta.m_DocData.len() { return Err(RustBladeError::InvalidFormat); }
            mergedDocData.extend_from_slice(&delta.m_DocData[..deltaDocBytes]);
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

        let headOffset = INDEX_FILE_HEADER_SIZE + PATH_PREFIX_SIDECAR_BYTES;
        let leafOffset = headOffset + headEntryCount as usize * 32;
        let docDataOffset = leafOffset + headEntryCount as usize * PAGE_SIZE;
        let indexOffset = docDataOffset + mergedDocData.len();
        let totalDocLength = (self.m_IndexFileHeader.IFH_AvgDocLength as f64 * baseDocCount as f64)
            + (delta.m_IndexFileHeader.IFH_AvgDocLength as f64 * deltaDocCount as f64);
        let avgDocLength = if mergedDocs == 0 { 1.0 } else { (totalDocLength / mergedDocs as f64) as f32 };

        let mut mergedLeafBlocks = Vec::with_capacity(headEntryCount as usize);
        if headEntryCount > 0 {
            let leafBytes = fs::read(&leafTempPath)?;
            if leafBytes.len() != headEntryCount as usize * PAGE_SIZE { return Err(RustBladeError::InvalidFormat); }
            for index in 0..headEntryCount as usize {
                let offset = index * PAGE_SIZE;
                mergedLeafBlocks.push(LeafTermBlock::from_bytes(&leafBytes[offset..offset + PAGE_SIZE]).ok_or(RustBladeError::InvalidFormat)?);
            }
        }
        let (mphfHeader, mphfDisplacements, mphfEntryPages): (TermMphfHeader, Vec<i32>, Vec<IndexBlock>) = (TermMphfHeader::default(), Vec::new(), Vec::new());
        let mphfHeaderCount = if !mphfDisplacements.is_empty() && !mphfEntryPages.is_empty() { 1u64 } else { 0u64 };
        let mphfHeaderOffset = indexOffset + indexBlockCount as usize * PAGE_SIZE;
        let mphfDisplacementOffset = mphfHeaderOffset + mphfHeaderCount as usize * TERM_MPHF_HEADER_SIZE;
        let mphfEntryOffset = mphfDisplacementOffset + mphfDisplacements.len() * std::mem::size_of::<i32>();

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
            IFH_TermMphfHeaderOffset: mphfHeaderOffset as u64,
            IFH_TermMphfHeaderCount: mphfHeaderCount,
            IFH_TermMphfDisplacementOffset: mphfDisplacementOffset as u64,
            IFH_TermMphfDisplacementCount: mphfDisplacements.len() as u64,
            IFH_TermMphfEntryOffset: mphfEntryOffset as u64,
            IFH_TermMphfEntryPageCount: mphfEntryPages.len() as u64,
        };

        let _ = fs::remove_file(&tempPath);

        {
            let mut output = File::create(&tempPath)?;
            output.write_all(&header.to_bytes())?;
            output.write_all(&IndexSerializer::EncodePathPrefixSidecar(&[]))?;
            Self::AppendFile(&mut output, &headTempPath)?;
            Self::AppendFile(&mut output, &leafTempPath)?;
            Self::AppendFile(&mut output, &docDataTempPath)?;
            Self::AppendFile(&mut output, &indexTempPath)?;
            if header.IFH_TermMphfHeaderCount > 0 {
                output.write_all(&mphfHeader.to_bytes())?;
                for displacement in &mphfDisplacements {
                    output.write_all(&displacement.to_le_bytes())?;
                }
                for page in &mphfEntryPages {
                    output.write_all(&page.IB_Data)?;
                }
            }
            output.flush()?;
        }

        Self::CleanupTempFiles(&[&headTempPath, &leafTempPath, &indexTempPath, &docDataTempPath]);
        let _ = fs::remove_file(output_path);
        fs::rename(&tempPath, output_path)?;

        self.LoadIndex(output_path)
    }

    fn BuildIndexData(store: &PostingStore, buildVectorIndex: bool) -> (IndexFileHeader, IndexBlockTable, HnswIndex, Vec<u8>, Vec<u8>, Vec<String>) {
        let blocks = IndexSerializer::BuildBlocks(store);
        let (docData, pathPrefixSidecar, pathPrefixes) = Self::EncodeDocData(store);
        let firstDocId = Self::StoreFirstDocId(store);
        let documentCount = Self::StoreDocDataRecordCount(store);

        let headOffset = INDEX_FILE_HEADER_SIZE + PATH_PREFIX_SIDECAR_BYTES;
        let leafOffset = headOffset + blocks.BBR_HeadTermEntries.len() * 32;
        let docDataOffset = leafOffset + blocks.BBR_LeafTermBlocks.len() * PAGE_SIZE;
        let indexOffset = docDataOffset + docData.len();
        let mphfHeaderCount = if !blocks.BBR_TermMphfDisplacements.is_empty() && !blocks.BBR_TermMphfEntryPages.is_empty() { 1u64 } else { 0u64 };
        let mphfHeaderOffset = indexOffset + blocks.BBR_IndexBlocks.len() * PAGE_SIZE;
        let mphfDisplacementOffset = mphfHeaderOffset + mphfHeaderCount as usize * TERM_MPHF_HEADER_SIZE;
        let mphfEntryOffset = mphfDisplacementOffset + blocks.BBR_TermMphfDisplacements.len() * std::mem::size_of::<i32>();

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
            IFH_TermMphfHeaderOffset: mphfHeaderOffset as u64,
            IFH_TermMphfHeaderCount: mphfHeaderCount,
            IFH_TermMphfDisplacementOffset: mphfDisplacementOffset as u64,
            IFH_TermMphfDisplacementCount: blocks.BBR_TermMphfDisplacements.len() as u64,
            IFH_TermMphfEntryOffset: mphfEntryOffset as u64,
            IFH_TermMphfEntryPageCount: blocks.BBR_TermMphfEntryPages.len() as u64,
        };

        let mut blockTable = IndexBlockTable::new(blocks.BBR_IndexBlocks.len().max(512) + 64);
        let mphfHeader = blocks.BBR_TermMphfHeader;
        let mphfDisplacements = blocks.BBR_TermMphfDisplacements;
        let mphfEntryPages = blocks.BBR_TermMphfEntryPages;
        blockTable.SetIndexBlocks(blocks.BBR_IndexBlocks);
        blockTable.SetHeadLeafTermTable(blocks.BBR_HeadTermEntries, blocks.BBR_LeafTermBlocks);
        if header.IFH_TermMphfHeaderCount > 0 {
            blockTable.SetTermMphf(mphfHeader, mphfDisplacements, mphfEntryPages);
        }

        let mut vectorIndex = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vectorIndex.SetDocDataWithFirstDocId(docData.clone(), firstDocId);
        if buildVectorIndex {
            for docId in store.AllDocStats().keys().copied() {
                if store.HasDocVector(docId) { vectorIndex.Add(docId); }
            }
        }

        (header, blockTable, vectorIndex, docData, pathPrefixSidecar, pathPrefixes)
    }

    fn BuildVectorRuntime(&mut self) {
        if self.m_VectorBuilt { return; }
        let mut vectorIndex = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        let firstDocId = Self::DocDataFirstDocId(&self.m_DocData, &self.m_IndexFileHeader);
        vectorIndex.SetDocDataWithFirstDocId(self.m_DocData.clone(), firstDocId);
        for slot in 0..self.m_IndexFileHeader.IFH_NumDocuments {
            let offset = slot as usize * DOC_REC_SIZE;
            if offset + DOC_REC_SIZE > self.m_DocData.len() { break; }
            let docId = firstDocId + slot;
            if u32::from_le_bytes(self.m_DocData[offset..offset + 4].try_into().unwrap()) as u64 == docId {
                vectorIndex.Add(docId);
            }
        }
        self.m_VectorIndex = vectorIndex;
        self.m_VectorBuilt = true;
    }

    fn EncodeDocData(store: &PostingStore) -> (Vec<u8>, Vec<u8>, Vec<String>) {
        let firstDocId = Self::StoreFirstDocId(store);
        let documentCount = Self::StoreDocDataRecordCount(store);
        let mut out = vec![0u8; documentCount as usize * DOC_REC_SIZE];
        let mut prefix_to_id = HashMap::new();
        let mut prefixes = Vec::new();
        let mut string_bytes = 0usize;
        for slot in 0..documentCount as usize {
            let offset = slot * DOC_REC_SIZE;
            out[offset..offset + 4].copy_from_slice(&u32::MAX.to_le_bytes());
        }
        for (docId, stats) in store.AllDocStats() {
            let offset = (*docId - firstDocId) as usize * DOC_REC_SIZE;
            let docId32 = (*docId).min(u32::MAX as u64) as u32;
            out[offset..offset + 4].copy_from_slice(&docId32.to_le_bytes());
            out[offset + 4..offset + 6].copy_from_slice(&DocDataEncodeScore(stats.importance).to_le_bytes());
            let doc_length = stats.doc_len.max(1) as f32;
            let diversity = (stats.unique_terms as f32 / doc_length).min(1.0);
            let log_length = doc_length.log2();
            let length_quality = (1.0 - (log_length - 6.0).abs() / 4.0).max(0.0);
            let quality_score = 0.6 * length_quality + 0.4 * diversity;
            let authority_score = if stats.title_len > 0 {
                ((1.0 + stats.title_len as f32).log2() / 5.0).min(1.0)
            } else {
                0.0
            };
            let repetition_penalty = (0.35 - diversity).max(0.0) / 0.35;
            let overlong_penalty = ((log_length - 10.0) / 4.0).max(0.0);
            let spam_score = (repetition_penalty + overlong_penalty).min(1.0);
            out[offset + 6..offset + 8].copy_from_slice(&DocDataEncodeScore(quality_score).to_le_bytes());
            out[offset + 14..offset + 16].copy_from_slice(&DocDataEncodeScore(authority_score).to_le_bytes());
            out[offset + 16..offset + 18].copy_from_slice(&DocDataEncodeScore(spam_score).to_le_bytes());
            out[offset + 26..offset + 30].copy_from_slice(&stats.title_len.to_le_bytes());
            out[offset + 30..offset + 34].copy_from_slice(&stats.body_len.to_le_bytes());
            out[offset + 34..offset + 38].copy_from_slice(&stats.url_len.to_le_bytes());
            out[offset + 38..offset + 42].copy_from_slice(&stats.anchor_len.to_le_bytes());
            out[offset + 42..offset + 46].copy_from_slice(&stats.meta_len.to_le_bytes());
            out[offset + 46..offset + 50].copy_from_slice(&diversity.to_le_bytes());
            out[offset + 50..offset + 54].copy_from_slice(&length_quality.to_le_bytes());
            if store.HasDocVector(*docId) {
                out[offset + 54..offset + 56].copy_from_slice(&(DOC_VECTOR_DIM as u16).to_le_bytes());
                out[offset + 56..offset + 58].copy_from_slice(&1u16.to_le_bytes());
                let vector = store.GetDocVector(*docId);
                for i in 0..DOC_VECTOR_DIM {
                    out[offset + DOC_VECTOR_OFFSET + i] = vector[i] as u8;
                }
            }
            Self::encode_doc_path(&mut out[offset..offset + DOC_REC_SIZE], &stats.path, &mut prefix_to_id, &mut prefixes, &mut string_bytes);
        }
        let sidecar = IndexSerializer::EncodePathPrefixSidecar(&prefixes);
        (out, sidecar, prefixes)
    }

    fn StoreFirstDocId(store: &PostingStore) -> u64 {
        store.AllDocStats().keys().copied().min().unwrap_or(0)
    }

    fn StoreDocDataRecordCount(store: &PostingStore) -> u64 {
        let Some(min_doc_id) = store.AllDocStats().keys().copied().min() else { return 0; };
        let max_doc_id = store.AllDocStats().keys().copied().max().unwrap_or(min_doc_id);
        max_doc_id - min_doc_id + 1
    }

    fn split_path_for_sidecar(full_path: &str) -> (String, String) {
        match full_path.rfind(['/', '\\']) {
            Some(index) => (full_path[..index].to_string(), full_path[index + 1..].to_string()),
            None => (String::new(), full_path.to_string()),
        }
    }

    fn path_prefix_sidecar_can_hold(prefix_count: usize, string_bytes: usize) -> bool {
        32 + prefix_count * 8 + string_bytes <= PATH_PREFIX_SIDECAR_BYTES
    }

    fn intern_path_prefix(prefix: &str, prefix_to_id: &mut HashMap<String, u16>, prefixes: &mut Vec<String>, string_bytes: &mut usize) -> u16 {
        if let Some(id) = prefix_to_id.get(prefix) { return *id; }
        if prefixes.len() >= DOC_PATH_PREFIX_INVALID as usize { return DOC_PATH_PREFIX_INVALID; }
        let next_string_bytes = *string_bytes + prefix.len();
        if !Self::path_prefix_sidecar_can_hold(prefixes.len() + 1, next_string_bytes) { return DOC_PATH_PREFIX_INVALID; }
        let id = prefixes.len() as u16;
        prefixes.push(prefix.to_string());
        prefix_to_id.insert(prefix.to_string(), id);
        *string_bytes = next_string_bytes;
        id
    }

    fn encode_doc_path(record: &mut [u8], full_path: &str, prefix_to_id: &mut HashMap<String, u16>, prefixes: &mut Vec<String>, string_bytes: &mut usize) {
        record[18..20].copy_from_slice(&0u16.to_le_bytes());
        record[DOC_PATH_OFFSET..DOC_PATH_OFFSET + DOC_PATH_MAX].fill(0);
        if full_path.is_empty() { return; }
        let (prefix, filename) = Self::split_path_for_sidecar(full_path);
        let prefix_id = Self::intern_path_prefix(&prefix, prefix_to_id, prefixes, string_bytes);
        let filename_len = filename.len().min(DOC_PATH_FILENAME_MAX);
        let path_len = (DOC_PATH_PREFIX_ID_BYTES + filename_len) as u16;
        record[18..20].copy_from_slice(&path_len.to_le_bytes());
        record[DOC_PATH_OFFSET..DOC_PATH_OFFSET + 2].copy_from_slice(&prefix_id.to_le_bytes());
        record[DOC_PATH_OFFSET + 2..DOC_PATH_OFFSET + 2 + filename_len].copy_from_slice(&filename.as_bytes()[..filename_len]);
    }

    fn DecodeDocPath(payload: &[u8], prefixes: &[String]) -> String {
        if payload.is_empty() { return String::new(); }
        if payload.len() < DOC_PATH_PREFIX_ID_BYTES {
            return std::str::from_utf8(payload).unwrap_or("").to_string();
        }
        let prefix_id = u16::from_le_bytes(payload[0..2].try_into().unwrap());
        let filename = std::str::from_utf8(&payload[2..]).unwrap_or("");
        if prefix_id == DOC_PATH_PREFIX_INVALID || prefix_id as usize >= prefixes.len() {
            return filename.to_string();
        }
        let prefix = &prefixes[prefix_id as usize];
        if prefix.is_empty() { return filename.to_string(); }
        let separator = if prefix.contains('\\') { '\\' } else { '/' };
        if prefix.ends_with(['/', '\\']) { format!("{prefix}{filename}") }
        else { format!("{prefix}{separator}{filename}") }
    }

    fn DocDataFirstDocId(docdata: &[u8], header: &IndexFileHeader) -> u64 {
        IndexSerializer::DocDataFirstDocId(docdata, header)
    }

    pub fn LoadIndex(&mut self, path: &str) -> Result<()> {
        self.StopSearchWorkers();
        self.m_IndexPath = Some(path.to_string());
        let mut store = PostingStore::new();
        let (header, head_term_entries, docdata) = IndexSerializer::load_file_tables(&mut store, path)?;
        let (pathPrefixSidecar, pathPrefixes) = IndexSerializer::LoadPathPrefixSidecar(path)?;
        let (mphfHeader, mphfDisplacements, mphfEntryPages) = IndexSerializer::LoadTermMphf(path, &header)?;
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
        if header.IFH_TermMphfHeaderCount > 0 {
            table.SetTermMphf(mphfHeader, mphfDisplacements, mphfEntryPages);
        }

        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocDataWithFirstDocId(docdata.clone(), Self::DocDataFirstDocId(&docdata, &header));

        self.m_Store       = Arc::new(RwLock::new(store));
        self.m_BlockTable = Arc::new(table);
        self.m_VectorIndex = vector_index;
        self.m_VectorBuilt = false;
        self.m_IndexFileHeader = header;
        self.m_DocData = docdata;
        self.m_PathPrefixSidecar = pathPrefixSidecar;
        self.m_PathPrefixes = pathPrefixes;
        self.m_Built       = true;
        self.m_LoadedFromDisk = true;
        self.LoadDeltaIndex();
        Ok(())
    }

    /// Load from raw bytes (WASM path — no file system access needed).
    pub fn LoadFromBytes(&mut self, data: &[u8]) -> Result<()> {
        let header = IndexFileHeader::parse(data)?;
        let mut store = PostingStore::new();
        let (head_term_entries, leaf_term_blocks, blocks, docdata, pathPrefixSidecar, pathPrefixes, mphfHeader, mphfDisplacements, mphfEntryPages) = IndexSerializer::decode(&mut store, data)?;
        let mut table = IndexBlockTable::new(blocks.len().max(512) + 64);
        table.SetIndexBlocks(blocks);
        table.SetHeadLeafTermTable(head_term_entries, leaf_term_blocks);
        if !mphfDisplacements.is_empty() && !mphfEntryPages.is_empty() {
            table.SetTermMphf(mphfHeader, mphfDisplacements, mphfEntryPages);
        }

        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocDataWithFirstDocId(docdata.clone(), Self::DocDataFirstDocId(&docdata, &IndexFileHeader { IFH_NumDocuments: (docdata.len() / DOC_REC_SIZE) as u64, ..IndexFileHeader::default() }));

        self.m_Store       = Arc::new(RwLock::new(store));
        self.m_BlockTable = Arc::new(table);
        self.m_VectorIndex = vector_index;
        self.m_VectorBuilt = false;
        self.m_IndexFileHeader = header;
        self.m_DocData = docdata;
        self.m_PathPrefixSidecar = pathPrefixSidecar;
        self.m_PathPrefixes = pathPrefixes;
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
                let docFreq = self.m_Store.read().unwrap().DocFreq(&tn.stream_key);
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

    fn BuildVectorIndexReader(&self, query: &[f32], efSearch: usize) -> Box<dyn IndexReader> {
        let results = self.m_VectorIndex.Search(query, 0, efSearch);
        Box::new(VectorIndexReader::new(results))
    }
}

impl Drop for IndexContext {
    fn drop(&mut self) {
        self.StopSearchWorkers();
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
    data[8..10].copy_from_slice(&entry.LTE_IndexOffset.to_le_bytes());
    data[10..12].copy_from_slice(&entry.LTE_IndexLength.to_le_bytes());
    data[12..14].copy_from_slice(&entry.LTE_ContinuationBlockCount.to_le_bytes());
    data[14] = entry.LTE_Flags;
    data[15] = entry.LTE_Term.len() as u8;
    data[16..16 + entry.LTE_Term.len()].copy_from_slice(entry.LTE_Term.as_bytes());
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
            LTE_IndexOffset: self.m_PostingIndexOffset as u16,
            LTE_IndexLength: self.m_PostingIndexLength as u16,
            LTE_ContinuationBlockCount: self.m_PostingContinuationBlockCount as u16,
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
