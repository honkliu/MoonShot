use std::sync::{Arc, Mutex};
use crate::posting_store::PostingStore;
use crate::block_table::IndexBlockTable;
use crate::index_writer::AdvancedIndexWriter;
use crate::eval_tree::{EvalTree, EvalNode};
use crate::advanced_reader::AdvancedIndexReader;
use crate::composite_readers::{AndIndexReader, OrIndexReader, NotIndexReader};
use crate::index_reader::IndexReader;
use crate::compiler::IndexSearchCompiler;
use crate::executor::{IndexSearchExecutor, SearchResult};
use crate::tokenizer::SmartTokenizer;
use crate::serializer::IndexSerializer;
use crate::error::Result;

/// Central factory — owns PostingStore, BlockTable and all search components.
/// Mirrors MoonShot's IndexContext.h.
pub struct IndexContext {
    store:       Arc<Mutex<PostingStore>>,
    block_table: Arc<IndexBlockTable>,
    tokenizer:   SmartTokenizer,
    compiler:    IndexSearchCompiler,
    index_path:  Option<String>,
    built:       bool,
}

impl IndexContext {
    pub fn new() -> Self { Self::with_path(None) }

    pub fn with_path(index_path: Option<String>) -> Self {
        let store = Arc::new(Mutex::new(PostingStore::new()));
        let mut ctx = Self {
            store:       Arc::clone(&store),
            block_table: Arc::new(IndexBlockTable::new(512)),
            tokenizer:   SmartTokenizer::new(),
            compiler:    IndexSearchCompiler::new(SmartTokenizer::new()),
            index_path:  index_path.clone(),
            built:       false,
        };
        if let Some(ref path) = index_path {
            let _ = ctx.load_index(path);
        }
        ctx
    }

    pub fn get_writer(&self) -> AdvancedIndexWriter {
        AdvancedIndexWriter::new(Arc::clone(&self.store))
    }

    pub fn get_tokenizer(&self) -> &SmartTokenizer { &self.tokenizer }
    pub fn get_compiler(&self)  -> &IndexSearchCompiler { &self.compiler }

    // ── Build (in-memory) ────────────────────────────────────────────────────

    /// Pack PostingStore entries into multi-term IndexBlocks (sorted alphabetically)
    /// and populate the Head/Leaf term table. Called lazily on first search.
    pub fn build(&mut self) {
        if self.built { return; }

        let store = self.store.lock().unwrap();

        let table = Arc::get_mut(&mut self.block_table)
            .expect("BlockTable must have no other refs during build");

        let (blocks, head_term_entries, leaf_term_blocks) = IndexSerializer::build_blocks_pub(&store);
        let n = blocks.len();
        *table = IndexBlockTable::new(n.max(512) + 64);

        table.set_index_blocks(blocks);
        table.set_head_leaf_term_table(head_term_entries, leaf_term_blocks);

        self.built = true;
    }

    fn ensure_built(&mut self) {
        if !self.built { self.build(); }
    }

    // ── Search ───────────────────────────────────────────────────────────────

    pub fn get_reader_for_query(&mut self, query: &str, stream_set: &str)
        -> Box<dyn IndexReader>
    {
        self.ensure_built();
        let tree = self.compiler.compile(query, stream_set);
        self.build_index_reader(tree.root)
    }

    pub fn get_reader(&mut self, tree: EvalTree) -> Box<dyn IndexReader> {
        self.ensure_built();
        self.build_index_reader(tree.root)
    }

    pub fn get_stream_reader(&mut self, stream_key: &str) -> Box<dyn IndexReader> {
        self.ensure_built();
        let doc_freq = self.store.lock().unwrap().doc_freq(stream_key);
        Box::new(AdvancedIndexReader::open(
            stream_key, Arc::clone(&self.block_table), doc_freq))
    }

    pub fn search(&mut self, query: &str, top_k: usize, stream_set: &str) -> Vec<SearchResult> {
        let mut reader = self.get_reader_for_query(query, stream_set);
        let store      = self.store.lock().unwrap();
        let executor   = IndexSearchExecutor::new(&store);
        executor.execute(reader.as_mut(), top_k)
    }

    // ── Persistence ─────────────────────────────────────────────────────────

    pub fn save_index(&mut self, path: &str) -> Result<()> {
        self.index_path = Some(path.to_string());
        let mut store = self.store.lock().unwrap();
        IndexSerializer::save(&mut store, path)
    }

    pub fn load_index(&mut self, path: &str) -> Result<()> {
        self.index_path = Some(path.to_string());
        let mut store = PostingStore::new();
        let (head_term_entries, leaf_term_blocks, blocks) = IndexSerializer::load(&mut store, path)?;
        let mut table = IndexBlockTable::new(blocks.len().max(512) + 64);
        table.set_index_blocks(blocks);
        table.set_head_leaf_term_table(head_term_entries, leaf_term_blocks);

        self.store       = Arc::new(Mutex::new(store));
        self.block_table = Arc::new(table);
        self.built       = true;
        Ok(())
    }

    /// Load from raw bytes (WASM path — no file system access needed).
    pub fn load_from_bytes(&mut self, data: &[u8]) -> Result<()> {
        let mut store = PostingStore::new();
        let (head_term_entries, leaf_term_blocks, blocks) = IndexSerializer::decode(&mut store, data)?;
        let mut table = IndexBlockTable::new(blocks.len().max(512) + 64);
        table.set_index_blocks(blocks);
        table.set_head_leaf_term_table(head_term_entries, leaf_term_blocks);

        self.store       = Arc::new(Mutex::new(store));
        self.block_table = Arc::new(table);
        self.built       = true;
        Ok(())
    }

    pub fn with_store<F, R>(&self, f: F) -> R
    where F: FnOnce(&PostingStore) -> R {
        let store = self.store.lock().unwrap();
        f(&store)
    }

    // ── ISR tree builder ─────────────────────────────────────────────────────

    fn build_index_reader(&self, node: Option<EvalNode>) -> Box<dyn IndexReader> {
        match node {
            None => Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0)),

            Some(EvalNode::Term(tn)) => {
                let doc_freq = self.store.lock().unwrap().doc_freq(&tn.stream_key);
                Box::new(AdvancedIndexReader::open(&tn.stream_key, Arc::clone(&self.block_table), doc_freq))
            }

            Some(EvalNode::And(an)) => {
                let children: Vec<Box<dyn IndexReader>> = an.children.into_iter()
                    .map(|c| self.build_index_reader(Some(c))).collect();
                if children.iter().any(|c| c.is_end()) {
                    return Box::new(AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0));
                }
                let mut v = children;
                if v.len() == 1 { return v.remove(0); }
                Box::new(AndIndexReader::new(v))
            }

            Some(EvalNode::Or(on)) => {
                let children: Vec<Box<dyn IndexReader>> = on.children.into_iter()
                    .map(|c| self.build_index_reader(Some(c)))
                    .filter(|r| !r.is_end()).collect();
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
