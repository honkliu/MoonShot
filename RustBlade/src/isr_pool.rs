use std::collections::HashMap;
use std::sync::Arc;
use crate::postings::PostingEntry;

// ---------------------------------------------------------------------------
// IsrPool — term-keyed cache of decoded posting lists.
//
// Mirrors REF's `SimpleISRPool` (HashMap<AtomHash, ISRWord*>).
//
// Multiple ISRs for the same term share a single Arc<Vec<PostingEntry>> so
// repeated opens of the same term cost only an Arc clone, not a heap copy.
// The pool is cleared between queries to prevent stale data across sessions.
// ---------------------------------------------------------------------------
#[derive(Default)]
pub struct IsrPool {
    cache: HashMap<String, Arc<Vec<PostingEntry>>>,
}

impl IsrPool {
    pub fn new() -> Self {
        Default::default()
    }

    /// Return the cached Arc for `term`, if any.
    pub fn get(&self, term: &str) -> Option<Arc<Vec<PostingEntry>>> {
        self.cache.get(term).cloned()
    }

    /// Insert `entries` for `term` and return an Arc to the stored data.
    pub fn insert(&mut self, term: String, entries: Vec<PostingEntry>) -> Arc<Vec<PostingEntry>> {
        let arc = Arc::new(entries);
        self.cache.insert(term, arc.clone());
        arc
    }

    pub fn contains(&self, term: &str) -> bool {
        self.cache.contains_key(term)
    }

    /// Drain the pool; call once per query boundary.
    pub fn clear(&mut self) {
        self.cache.clear();
    }

    pub fn len(&self) -> usize {
        self.cache.len()
    }
}
