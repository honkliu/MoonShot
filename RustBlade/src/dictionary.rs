use std::collections::HashMap;
use serde::{Deserialize, Serialize};
use crate::postings::PostingList;

// ---------------------------------------------------------------------------
// Per-document statistics needed for BM25.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy, Serialize, Deserialize)]
pub struct DocStats {
    /// Number of indexed tokens in this document.
    pub doc_len: u32,
}

// ---------------------------------------------------------------------------
// TermDictionary — maps term strings to their PostingList.
//
// Mirrors MoonShot's TermToBlock (but in-memory) and REF's atom→ISR mapping.
// Built once by InvertedIndexBuilder, then read-only at query time.
// ---------------------------------------------------------------------------
#[derive(Default, Serialize, Deserialize)]
pub struct TermDictionary {
    postings:    HashMap<String, PostingList>,
    doc_stats:   HashMap<u64, DocStats>,
    total_docs:  u64,
    total_terms: u64,  // sum of all doc lengths — needed for avg_doc_len
}

impl TermDictionary {
    pub fn new() -> Self {
        Default::default()
    }

    // -- building -----------------------------------------------------------

    pub fn insert_posting(&mut self, term: String, list: PostingList) {
        self.postings.insert(term, list);
    }

    pub fn insert_doc_stats(&mut self, doc_id: u64, stats: DocStats) {
        self.total_terms += stats.doc_len as u64;
        self.total_docs  += 1;
        self.doc_stats.insert(doc_id, stats);
    }

    // Overwrite global counters explicitly (used after deserialization).
    pub fn set_global_stats(&mut self, total_docs: u64, total_terms: u64) {
        self.total_docs  = total_docs;
        self.total_terms = total_terms;
    }

    // -- reading ------------------------------------------------------------

    pub fn get_posting(&self, term: &str) -> Option<&PostingList> {
        self.postings.get(term)
    }

    pub fn doc_freq(&self, term: &str) -> u32 {
        self.postings.get(term).map(|l| l.doc_freq).unwrap_or(0)
    }

    pub fn doc_stats(&self, doc_id: u64) -> Option<DocStats> {
        self.doc_stats.get(&doc_id).copied()
    }

    pub fn doc_len(&self, doc_id: u64) -> u32 {
        self.doc_stats.get(&doc_id).map(|s| s.doc_len).unwrap_or(1)
    }

    pub fn total_docs(&self) -> u64 {
        self.total_docs
    }

    pub fn avg_doc_len(&self) -> f32 {
        if self.total_docs == 0 { 1.0 } else { self.total_terms as f32 / self.total_docs as f32 }
    }

    pub fn term_count(&self) -> usize {
        self.postings.len()
    }
}
