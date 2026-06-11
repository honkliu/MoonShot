use std::sync::{Arc, Mutex};
use crate::posting_store::PostingStore;
use crate::block_table::IndexBlockTable;
use crate::index_writer::{IndexWriter, AdvancedIndexWriter};
use crate::eval_tree::{EvalTree, EvalNode, TermNode, AndNode, OrNode, NotNode};
use crate::advanced_reader::AdvancedIndexReader;
use crate::composite_readers::{AndIndexReader, OrIndexReader, NotIndexReader};
use crate::index_reader::IndexReader;
use crate::compiler::IndexSearchCompiler;
use crate::executor::{IndexSearchExecutor, SearchResult};
use crate::tokenizer::SmartTokenizer;
use crate::serializer::IndexSerializer;
use crate::error::Result;

/*
* IndexContext — central factory that owns all search engine components.
*
* Write path:  get_writer() → AdvancedIndexWriter → PostingStore
* Build step:  build() encodes PostingStore entries into IndexBlocks,
*              populates BlockTable + TermToBlock mapping.
* Read path:   get_reader(EvalTree) → ISR tree
*              BuildIndexReader recursively:
*                TermNode → AdvancedIndexReader  (leaf, = REF ISRWord)
*                AndNode  → AndIndexReader
*                OrNode   → OrIndexReader
*                NotNode  → NotIndexReader
*
* Mirrors MoonShot's IndexContext.h.
*/
pub struct IndexContext {
    store:       Arc<Mutex<PostingStore>>,
    block_table: Arc<IndexBlockTable>,
    tokenizer:   SmartTokenizer,
    compiler:    IndexSearchCompiler,
    index_path:  Option<String>,
    built:       bool,
}

impl IndexContext {
    pub fn new() -> Self {
        Self::with_path(None)
    }

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

    pub fn build(&mut self) {
        if self.built { return; }

        let block_capacity = crate::block_table::DATA_SIZE - 1;
        let mut block_seq  = 0u32;

        let store = self.store.lock().unwrap();

        /* SAFETY: no readers exist yet — we're the only Arc holder at build time.
         * If other Arcs exist (shouldn't during build), this will panic. */
        let table = Arc::get_mut(&mut self.block_table)
            .expect("BlockTable should have no other references during build");

        for (stream_key, posting_list) in store.all_postings() {
            let bytes = posting_list.get_bytes_ref();
            if bytes.is_empty() { continue; }

            let num_blocks = (bytes.len() + block_capacity - 1) / block_capacity;
            let first_seq  = block_seq;

            for blk in 0..num_blocks {
                let offset   = blk * block_capacity;
                let end      = (offset + block_capacity).min(bytes.len());
                let has_more = blk + 1 < num_blocks;
                table.insert_block(block_seq, &bytes[offset..end], has_more);
                block_seq += 1;
            }

            table.add_term_mapping(stream_key, first_seq);
        }

        self.built = true;
    }

    fn ensure_built(&mut self) {
        if !self.built { self.build(); }
    }

    /* Compile a query string and return the ISR tree. */
    pub fn get_reader_for_query(
        &mut self,
        query:      &str,
        stream_set: &str,
    ) -> Box<dyn IndexReader> {
        self.ensure_built();
        let tree = self.compiler.compile(query, stream_set);
        self.build_index_reader(tree.root)
    }

    /* Build an ISR tree from an already-compiled EvalTree. */
    pub fn get_reader(&mut self, tree: EvalTree) -> Box<dyn IndexReader> {
        self.ensure_built();
        self.build_index_reader(tree.root)
    }

    /* Open a reader for a specific stream key (debug / low-level access). */
    pub fn get_stream_reader(&mut self, stream_key: &str) -> Box<dyn IndexReader> {
        self.ensure_built();
        let doc_freq = self.store.lock().unwrap().doc_freq(stream_key);
        Box::new(AdvancedIndexReader::open(
            stream_key,
            Arc::clone(&self.block_table),
            doc_freq,
        ))
    }

    /* Run a query and return ranked results in one call. */
    pub fn search(
        &mut self,
        query:      &str,
        top_k:      usize,
        stream_set: &str,
    ) -> Vec<SearchResult> {
        let mut reader = self.get_reader_for_query(query, stream_set);
        let store      = self.store.lock().unwrap();
        let executor   = IndexSearchExecutor::new(&store);
        executor.execute(reader.as_mut(), top_k)
    }

    pub fn save_index(&mut self, path: &str) -> Result<()> {
        self.index_path = Some(path.to_string());
        let mut store = self.store.lock().unwrap();
        IndexSerializer::save(&mut store, path)
    }

    pub fn load_index(&mut self, path: &str) -> Result<()> {
        self.index_path = Some(path.to_string());
        let mut store = PostingStore::new();
        IndexSerializer::load(&mut store, path)?;
        self.store = Arc::new(Mutex::new(store));
        self.block_table = Arc::new(IndexBlockTable::new(512));
        self.built = false;
        Ok(())
    }

    /* Borrow the PostingStore for direct access. */
    pub fn with_store<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&PostingStore) -> R,
    {
        let store = self.store.lock().unwrap();
        f(&store)
    }

    /* ------------------------------------------------------------------ */

    fn build_index_reader(&self, node: Option<EvalNode>) -> Box<dyn IndexReader> {
        match node {
            None => {
                let empty = AdvancedIndexReader::open("", Arc::clone(&self.block_table), 0);
                Box::new(empty)
            }

            Some(EvalNode::Term(term_node)) => {
                let doc_freq = self.store.lock().unwrap()
                    .doc_freq(&term_node.stream_key);
                Box::new(AdvancedIndexReader::open(
                    &term_node.stream_key,
                    Arc::clone(&self.block_table),
                    doc_freq,
                ))
            }

            Some(EvalNode::And(and_node)) => {
                let mut children: Vec<Box<dyn IndexReader>> = and_node.children
                    .into_iter()
                    .map(|c| self.build_index_reader(Some(c)))
                    .collect();

                /* prune empty leaves — any empty child makes the whole AND empty */
                if children.iter().any(|c| c.is_end()) {
                    return Box::new(AdvancedIndexReader::open(
                        "", Arc::clone(&self.block_table), 0));
                }

                if children.len() == 1 { return children.remove(0); }
                Box::new(AndIndexReader::new(children))
            }

            Some(EvalNode::Or(or_node)) => {
                let children: Vec<Box<dyn IndexReader>> = or_node.children
                    .into_iter()
                    .map(|c| self.build_index_reader(Some(c)))
                    .filter(|r| !r.is_end())    /* prune empty leaves */
                    .collect();

                if children.is_empty() {
                    return Box::new(AdvancedIndexReader::open(
                        "", Arc::clone(&self.block_table), 0));
                }
                if children.len() == 1 {
                    return children.into_iter().next().unwrap();
                }
                Box::new(OrIndexReader::new(children))
            }

            Some(EvalNode::Not(not_node)) => {
                let base    = self.build_index_reader(Some(*not_node.base));
                let exclude = self.build_index_reader(Some(*not_node.exclude));
                Box::new(NotIndexReader::new(base, exclude))
            }
        }
    }
}

impl Default for IndexContext {
    fn default() -> Self { Self::new() }
}
