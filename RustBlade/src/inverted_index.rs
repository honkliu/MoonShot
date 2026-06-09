use std::collections::HashMap;
use std::sync::Arc;
use serde::{Deserialize, Serialize};

use crate::clock_cache::ClockCache;
use crate::dictionary::{DocStats, TermDictionary};
use crate::isr::{AndIsr, InvertedIsr, Isr, NotIsr, OrIsr};
use crate::isr_pool::IsrPool;
use crate::postings::{PostingEntry, PostingListBuilder};
use crate::tokenizer::Tokenizer;

// ---------------------------------------------------------------------------
// InvertedIndexBuilder — accumulates raw postings, then compiles an index.
//
// Mirrors MoonShot's AdvancedIndexWriter (real write path) and
// REF's IndexAccessBase construction flow.
// ---------------------------------------------------------------------------
#[derive(Default)]
pub struct InvertedIndexBuilder {
    /// term → sorted list of (doc_id, tf)
    raw: HashMap<String, Vec<(u64, u32)>>,
    doc_stats: HashMap<u64, DocStats>,
}

impl InvertedIndexBuilder {
    pub fn new() -> Self {
        Default::default()
    }

    /// Tokenize `text`, count term frequencies, and add to the index.
    pub fn add_document(&mut self, doc_id: u64, tokens: Vec<String>) {
        let doc_len = tokens.len() as u32;

        let mut tf_map: HashMap<String, u32> = HashMap::new();
        for token in tokens {
            *tf_map.entry(token).or_insert(0) += 1;
        }

        for (term, tf) in tf_map {
            self.raw.entry(term).or_default().push((doc_id, tf));
        }

        // Accumulate doc length stats.
        let entry = self.doc_stats.entry(doc_id).or_insert(DocStats { doc_len: 0 });
        entry.doc_len += doc_len;
    }

    /// Finalize and return a read-only `InvertedIndex`.
    pub fn build(mut self) -> InvertedIndex {
        let mut dict = TermDictionary::new();

        for (doc_id, stats) in &self.doc_stats {
            dict.insert_doc_stats(*doc_id, *stats);
        }

        for (term, mut entries) in self.raw.drain() {
            entries.sort_by_key(|e| e.0);
            let mut builder = PostingListBuilder::new();
            for (doc_id, tf) in entries {
                builder.push(doc_id, tf);
            }
            dict.insert_posting(term, builder.build());
        }

        InvertedIndex {
            dict,
            isr_pool: IsrPool::new(),
            block_cache: ClockCache::new(512),
        }
    }
}

// ---------------------------------------------------------------------------
// InvertedIndex — the read-only, query-time index structure.
//
// The two-level cache (clock cache → ISR pool) mirrors MoonShot's
// BlockTable/BlockCache + REF's SimpleISRPool.
// ---------------------------------------------------------------------------
pub struct InvertedIndex {
    pub dict:        TermDictionary,
    /// Per-query ISR pool: shared Arc slices so the same posting list is not
    /// decoded twice within one query execution.
    pub isr_pool:    IsrPool,
    /// Clock-replacement cache mapping term → Arc<Vec<PostingEntry>>.
    /// Survives across queries; the ISR pool is per-query on top of it.
    block_cache: ClockCache<String, Arc<Vec<PostingEntry>>>,
}

impl InvertedIndex {
    // -- ISR creation -------------------------------------------------------

    /// Open an ISR for `term`.  Returns None if the term is not indexed.
    ///
    /// Lookup order: ISR pool → clock cache → TermDictionary.
    pub fn open_isr(&mut self, term: &str) -> Option<Box<dyn Isr>> {
        let df = self.dict.doc_freq(term);
        if df == 0 {
            return None;
        }
        let entries = self.load_entries(term)?;
        Some(Box::new(InvertedIsr::new(entries, df)))
    }

    /// Batch-open ISRs for multiple terms (mirrors REF's GetPostingListInBatch).
    pub fn open_isrs(&mut self, terms: &[&str]) -> Vec<Option<Box<dyn Isr>>> {
        terms.iter().map(|t| self.open_isr(t)).collect()
    }

    // -- stats --------------------------------------------------------------

    pub fn doc_count(&self) -> u64  { self.dict.total_docs() }
    pub fn avg_doc_len(&self) -> f32 { self.dict.avg_doc_len() }
    pub fn doc_freq(&self, term: &str) -> u32 { self.dict.doc_freq(term) }
    pub fn doc_len(&self, doc_id: u64) -> u32 { self.dict.doc_len(doc_id) }

    /// Reset the per-query ISR pool; call at the start of each new query.
    pub fn reset_isr_pool(&mut self) {
        self.isr_pool.clear();
    }

    // -- private ------------------------------------------------------------

    fn load_entries(&mut self, term: &str) -> Option<Arc<Vec<PostingEntry>>> {
        // 1. ISR pool (per-query)
        if let Some(arc) = self.isr_pool.get(term) {
            return Some(arc);
        }
        // 2. Clock cache (cross-query)
        if let Some(arc) = self.block_cache.get(&term.to_string()) {
            let arc2 = self.isr_pool.insert(term.to_string(), (*arc).clone());
            return Some(arc2);
        }
        // 3. TermDictionary (cold path)
        let entries: Vec<PostingEntry> = self.dict.get_posting(term)?.decoded.clone();
        let arc = Arc::new(entries.clone());
        self.block_cache.insert(term.to_string(), arc.clone());
        let arc2 = self.isr_pool.insert(term.to_string(), entries);
        Some(arc2)
    }
}

// ---------------------------------------------------------------------------
// Serializable snapshot for persistence.
// ---------------------------------------------------------------------------
#[derive(Serialize, Deserialize)]
pub struct InvertedIndexSnapshot {
    pub dict: TermDictionary,
}

impl From<&InvertedIndex> for InvertedIndexSnapshot {
    fn from(idx: &InvertedIndex) -> Self {
        // Clone the dictionary by serializing/deserializing through serde.
        // (TermDictionary derives Serialize+Deserialize, so this is clean.)
        let bytes = serde_json::to_vec(&idx.dict).expect("dict serialize");
        let dict: TermDictionary = serde_json::from_slice(&bytes).expect("dict deserialize");
        Self { dict }
    }
}

impl From<InvertedIndexSnapshot> for InvertedIndex {
    fn from(snap: InvertedIndexSnapshot) -> Self {
        InvertedIndex {
            dict:        snap.dict,
            isr_pool:    IsrPool::new(),
            block_cache: ClockCache::new(512),
        }
    }
}
