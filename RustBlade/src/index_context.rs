use std::sync::{Arc, Mutex};
use crate::posting_store::PostingStore;
use crate::block_table::IndexBlockTable;
use crate::index_writer::AdvancedIndexWriter;
use crate::eval_tree::{EvalTree, EvalNode};
use crate::advanced_reader::AdvancedIndexReader;
use crate::composite_readers::{AndIndexReader, OrIndexReader, NotIndexReader};
use crate::index_reader::IndexReader;
use crate::compiler::IndexSearchCompiler;
use crate::tokenizer::SmartTokenizer;
use crate::serializer::IndexSerializer;
use crate::serializer::IndexFileHeader;
use crate::error::Result;
use crate::block_table::{DOC_PATH_MAX, DOC_REC_SIZE, DOC_VECTOR_DIM, INDEX_FILE_HEADER_SIZE, PAGE_SIZE};
use crate::vector_index::{HnswIndex, Metric as VectorMetric};

/// Central factory — owns PostingStore, BlockTable and all search components.
/// Mirrors MoonShot's IndexContext.h.
pub struct IndexContext {
    store:       Arc<Mutex<PostingStore>>,
    block_table: Arc<IndexBlockTable>,
    tokenizer:   SmartTokenizer,
    compiler:    IndexSearchCompiler,
    vector_index: HnswIndex,
    index_path:  Option<String>,
    built:       bool,
    write_block_table: IndexBlockTable,
    write_vector_index: HnswIndex,
    write_header: IndexFileHeader,
    write_docdata: Vec<u8>,
}

#[allow(non_snake_case)]
impl IndexContext {
    pub fn new() -> Self { Self::with_path(None) }

    pub fn with_path(index_path: Option<String>) -> Self {
        let store = Arc::new(Mutex::new(PostingStore::new()));
        let mut ctx = Self {
            store:       Arc::clone(&store),
            block_table: Arc::new(IndexBlockTable::new(512)),
            tokenizer:   SmartTokenizer::new(),
            compiler:    IndexSearchCompiler::new(SmartTokenizer::new()),
            vector_index: HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine),
            index_path:  index_path.clone(),
            built:       false,
            write_block_table: IndexBlockTable::new(512),
            write_vector_index: HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine),
            write_header: IndexFileHeader::default(),
            write_docdata: Vec::new(),
        };
        if let Some(ref path) = index_path {
            let _ = ctx.LoadIndex(path);
        }
        ctx
    }

    pub fn GetWriter(&self) -> AdvancedIndexWriter {
        AdvancedIndexWriter::new(Arc::clone(&self.store))
    }

    pub fn GetTokenizer(&self) -> &SmartTokenizer { &self.tokenizer }
    pub fn GetCompiler(&self)  -> &IndexSearchCompiler { &self.compiler }
    pub fn GetStore(&self) -> Arc<Mutex<PostingStore>> { Arc::clone(&self.store) }
    pub fn DocumentCount(&self) -> u64 { self.store.lock().unwrap().TotalDocs() }
    pub fn AvgDocLen(&self) -> f32 { self.store.lock().unwrap().AvgDocLen() }
    pub fn GetDocPath(&self, doc_id: u64) -> String { self.store.lock().unwrap().GetDocPath(doc_id).to_string() }

    // ── Build (in-memory) ────────────────────────────────────────────────────

    /// Pack PostingStore entries into multi-term IndexBlocks (sorted alphabetically)
    /// and populate the Head/Leaf term table. Called lazily on first search.
    pub fn Build(&mut self) {
        if self.built { return; }

        let store = self.store.lock().unwrap();

        let table = Arc::get_mut(&mut self.block_table)
            .expect("BlockTable must have no other refs during build");

        let (header, new_table, vector_index, _) = Self::build_index_data(&store);
        *table = new_table;
        self.vector_index = vector_index;

        let _ = header;
        self.built = true;
    }

    fn ensure_built(&mut self) {
        if !self.built { self.Build(); }
    }

    // ── Search ───────────────────────────────────────────────────────────────

    pub fn Compile(&self, query: &str, stream_set: &str) -> EvalTree {
        self.compiler.Compile(query, stream_set)
    }

    pub fn GetReaderForQuery(&mut self, query: &str, stream_set: &str)
        -> Box<dyn IndexReader>
    {
        self.ensure_built();
        let tree = self.Compile(query, stream_set);
        self.build_index_reader(tree.root)
    }

    pub fn GetReader(&mut self, tree: EvalTree) -> Box<dyn IndexReader> {
        self.ensure_built();
        self.build_index_reader(tree.root)
    }

    pub fn GetStreamReader(&mut self, stream_key: &str) -> Box<dyn IndexReader> {
        self.ensure_built();
        let doc_freq = self.store.lock().unwrap().DocFreq(stream_key);
        Box::new(AdvancedIndexReader::open(
            stream_key, Arc::clone(&self.block_table), doc_freq))
    }

    pub fn VectorSearch(&self, query: &[f32], top_k: usize, ef_search: usize) -> Vec<(u64, f32)> {
        self.vector_index.Search(query, top_k, ef_search)
    }

    // ── Persistence ─────────────────────────────────────────────────────────

    pub fn SaveIndex(&mut self, path: &str) -> Result<()> {
        self.index_path = Some(path.to_string());
        let (header, block_table, vector_index, docdata) = {
            let store = self.store.lock().unwrap();
            Self::build_index_data(&store)
        };

        self.write_header = header;
        self.write_block_table = block_table;
        self.write_vector_index = vector_index;
        self.write_docdata = docdata;

        IndexSerializer::Save(&self.write_header, &self.write_block_table, &self.write_docdata, path)
    }

    fn build_index_data(store: &PostingStore) -> (IndexFileHeader, IndexBlockTable, HnswIndex, Vec<u8>) {
        let blocks = IndexSerializer::BuildBlocks(store);
        let docdata = Self::encode_docdata(store);
        let document_count = store.AllDocStats().keys().copied().max().map(|id| id + 1).unwrap_or(0);

        let head_offset = INDEX_FILE_HEADER_SIZE;
        let leaf_offset = head_offset + blocks.BBR_HeadTermEntries.len() * 32;
        let docdata_offset = leaf_offset + blocks.BBR_LeafTermBlocks.len() * PAGE_SIZE;
        let index_offset = docdata_offset + docdata.len();

        let header = IndexFileHeader {
            ifh_avg_doc_length: store.AvgDocLen(),
            ifh_num_documents: document_count,
            ifh_num_terms: blocks.BBR_TotalTerms,
            ifh_head_term_entry_offset: head_offset as u64,
            ifh_head_term_entry_count: blocks.BBR_HeadTermEntries.len() as u64,
            ifh_leaf_term_block_offset: leaf_offset as u64,
            ifh_leaf_term_block_count: blocks.BBR_LeafTermBlocks.len() as u64,
            ifh_doc_data_offset: docdata_offset as u64,
            ifh_index_block_offset: index_offset as u64,
            ifh_index_block_count: blocks.BBR_IndexBlocks.len() as u64,
        };

        let mut block_table = IndexBlockTable::new(blocks.BBR_IndexBlocks.len().max(512) + 64);
        block_table.SetIndexBlocks(blocks.BBR_IndexBlocks);
        block_table.SetHeadLeafTermTable(blocks.BBR_HeadTermEntries, blocks.BBR_LeafTermBlocks);

        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocData(docdata.clone());
        for doc_id in 0..header.ifh_num_documents { vector_index.Add(doc_id); }

        (header, block_table, vector_index, docdata)
    }

    fn encode_docdata(store: &PostingStore) -> Vec<u8> {
        let document_count = store.AllDocStats().keys().copied().max().map(|id| id + 1).unwrap_or(0);
        let mut out = vec![0u8; document_count as usize * DOC_REC_SIZE];
        for (doc_id, stats) in store.AllDocStats() {
            let offset = *doc_id as usize * DOC_REC_SIZE;
            out[offset..offset + 8].copy_from_slice(&doc_id.to_le_bytes());
            out[offset + 32..offset + 36].copy_from_slice(&stats.doc_len.to_le_bytes());
            out[offset + 44..offset + 48].copy_from_slice(&stats.importance.to_le_bytes());
            out[offset + 144..offset + 146].copy_from_slice(&(DOC_VECTOR_DIM as u16).to_le_bytes());
            out[offset + 146..offset + 148].copy_from_slice(&1u16.to_le_bytes());
            let vector = store.GetDocVector(*doc_id);
            for i in 0..DOC_VECTOR_DIM {
                out[offset + 256 + i] = vector[i] as u8;
            }
            let path_len = stats.path.len().min(DOC_PATH_MAX);
            out[offset + 72..offset + 74].copy_from_slice(&(path_len as u16).to_le_bytes());
            out[offset + 768..offset + 768 + path_len].copy_from_slice(&stats.path.as_bytes()[..path_len]);
        }
        out
    }

    pub fn LoadIndex(&mut self, path: &str) -> Result<()> {
        self.index_path = Some(path.to_string());
        let mut store = PostingStore::new();
        let (header, head_term_entries, docdata) = IndexSerializer::load_file_tables(&mut store, path)?;
        let mut table = IndexBlockTable::new(header.ifh_index_block_count as usize);
        table.InitFileBacked(
            path,
            header.ifh_index_block_offset,
            header.ifh_index_block_count as u32,
            header.ifh_index_block_count.min(25_600) as u32,
            header.ifh_leaf_term_block_offset,
            header.ifh_leaf_term_block_count as u32,
            header.ifh_leaf_term_block_count.min(25_600) as u32,
        )?;
        table.SetHeadEntries(head_term_entries);

        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocData(docdata);
        for doc_id in 0..header.ifh_num_documents { vector_index.Add(doc_id); }

        self.store       = Arc::new(Mutex::new(store));
        self.block_table = Arc::new(table);
        self.vector_index = vector_index;
        self.built       = true;
        Ok(())
    }

    /// Load from raw bytes (WASM path — no file system access needed).
    pub fn LoadFromBytes(&mut self, data: &[u8]) -> Result<()> {
        let mut store = PostingStore::new();
        let (head_term_entries, leaf_term_blocks, blocks, docdata) = IndexSerializer::decode(&mut store, data)?;
        let mut table = IndexBlockTable::new(blocks.len().max(512) + 64);
        table.SetIndexBlocks(blocks);
        table.SetHeadLeafTermTable(head_term_entries, leaf_term_blocks);

        let doc_count = store.TotalDocs();
        let mut vector_index = HnswIndex::new(DOC_VECTOR_DIM, 32, 200, VectorMetric::Cosine);
        vector_index.SetDocData(docdata);
        for doc_id in 0..doc_count { vector_index.Add(doc_id); }

        self.store       = Arc::new(Mutex::new(store));
        self.block_table = Arc::new(table);
        self.vector_index = vector_index;
        self.built       = true;
        Ok(())
    }

    // ── ISR tree builder ─────────────────────────────────────────────────────

    fn build_index_reader(&self, node: Option<EvalNode>) -> Box<dyn IndexReader> {
        match node {
            None => Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0)),

            Some(EvalNode::Term(tn)) => {
                let doc_freq = self.store.lock().unwrap().DocFreq(&tn.stream_key);
                Box::new(AdvancedIndexReader::open(&tn.stream_key, Arc::clone(&self.block_table), doc_freq))
            }

            Some(EvalNode::And(an)) => {
                let children: Vec<Box<dyn IndexReader>> = an.children.into_iter()
                    .map(|c| self.build_index_reader(Some(c))).collect();
                if children.iter().any(|c| c.IsEnd()) {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0));
                }
                let mut v = children;
                if v.len() == 1 { return v.remove(0); }
                Box::new(AndIndexReader::new(v))
            }

            Some(EvalNode::Or(on)) => {
                let children: Vec<Box<dyn IndexReader>> = on.children.into_iter()
                    .map(|c| self.build_index_reader(Some(c)))
                    .filter(|r| !r.IsEnd()).collect();
                if children.is_empty() {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0));
                }
                let mut v = children;
                if v.len() == 1 { return v.remove(0); }
                Box::new(OrIndexReader::new(v))
            }

            Some(EvalNode::Not(nn)) => {
                let base    = self.build_index_reader(Some(*nn.base));
                let exclude = self.build_index_reader(Some(*nn.exclude));
                Box::new(NotIndexReader::new(base, exclude))
            }
        }
    }
}


impl Default for IndexContext {
    fn default() -> Self { Self::new() }
}
