use std::sync::Arc;
use crate::bm25::Bm25Scorer;
use crate::postings::PostingEntry;

/// Sentinel value: the ISR is exhausted / past the end of the posting list.
pub const DONE: u64 = u64::MAX;

// ---------------------------------------------------------------------------
// Isr trait — Index Sequence Reader
//
// Mirrors MoonShot's `IndexReader` and REF's `ISR`/`ISRWord` interface.
// Each ISR walks a posting list (or a compound of multiple lists) in
// ascending doc_id order.
//
// The `bm25_score` method lets the ISR tree compute relevance scores
// without requiring the executor to know the tree structure.
// ---------------------------------------------------------------------------
pub trait Isr: Send {
    /// Current document ID, or DONE if exhausted.
    fn current_doc(&self) -> u64;

    /// Term frequency for the current document.
    /// For compound ISRs (And/Or) this is the sum of child TFs.
    fn term_freq(&self) -> u32;

    /// Advance to the next matching document.
    fn advance(&mut self);

    /// Skip forward so that `current_doc() >= target`.
    fn seek(&mut self, target: u64);

    fn is_done(&self) -> bool {
        self.current_doc() == DONE
    }

    /// BM25 contribution of this node for `current_doc()`.
    /// `doc_len` is the length of the current document.
    fn bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32;
}

// ---------------------------------------------------------------------------
// InvertedIsr — single-term posting list reader.
//
// Holds an Arc to the decoded entries so the same data is shared across
// concurrent ISR pool accesses (equivalent to REF's SimpleISRPool).
// ---------------------------------------------------------------------------
pub struct InvertedIsr {
    entries:  Arc<Vec<PostingEntry>>,
    pos:      usize,
    doc_freq: u32,
}

impl InvertedIsr {
    pub fn new(entries: Arc<Vec<PostingEntry>>, doc_freq: u32) -> Self {
        Self { entries, pos: 0, doc_freq }
    }
}

impl Isr for InvertedIsr {
    fn current_doc(&self) -> u64 {
        self.entries.get(self.pos).map(|e| e.doc_id).unwrap_or(DONE)
    }

    fn term_freq(&self) -> u32 {
        self.entries.get(self.pos).map(|e| e.term_freq).unwrap_or(0)
    }

    fn advance(&mut self) {
        if self.pos < self.entries.len() {
            self.pos += 1;
        }
    }

    fn seek(&mut self, target: u64) {
        // Binary search from the current position — O(log n).
        let slice = &self.entries[self.pos..];
        match slice.binary_search_by_key(&target, |e| e.doc_id) {
            Ok(i)  => self.pos += i,
            Err(i) => self.pos += i,
        }
    }

    fn bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        scorer.score(self.term_freq(), doc_len, self.doc_freq)
    }
}

// ---------------------------------------------------------------------------
// AndIsr — DAAT (Document-At-A-Time) intersection.
//
// All children must be at the same doc_id; advance moves past the current
// match and realigns.
// ---------------------------------------------------------------------------
pub struct AndIsr {
    children: Vec<Box<dyn Isr>>,
}

impl AndIsr {
    pub fn new(children: Vec<Box<dyn Isr>>) -> Self {
        let mut s = Self { children };
        s.align();
        s
    }

    /// Advance all children to the global maximum, repeating until all agree.
    fn align(&mut self) {
        loop {
            let max = self.children.iter().map(|c| c.current_doc()).max().unwrap_or(DONE);
            if max == DONE {
                return;
            }
            let all_match = self.children.iter().all(|c| c.current_doc() == max);
            if all_match {
                return;
            }
            for child in &mut self.children {
                child.seek(max);
            }
        }
    }
}

impl Isr for AndIsr {
    fn current_doc(&self) -> u64 {
        self.children.iter().map(|c| c.current_doc()).max().unwrap_or(DONE)
    }

    fn term_freq(&self) -> u32 {
        self.children.iter().map(|c| c.term_freq()).sum()
    }

    fn advance(&mut self) {
        let doc = self.current_doc();
        if doc == DONE {
            return;
        }
        // Advance every child that is sitting on the current doc.
        for child in &mut self.children {
            if child.current_doc() == doc {
                child.advance();
            }
        }
        self.align();
    }

    fn seek(&mut self, target: u64) {
        for child in &mut self.children {
            child.seek(target);
        }
        self.align();
    }

    fn bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.children.iter().map(|c| c.bm25_score(scorer, doc_len)).sum()
    }
}

// ---------------------------------------------------------------------------
// OrIsr — DAAT union.
//
// Returns the minimum current doc_id across children; advances all children
// sitting on that doc.
// ---------------------------------------------------------------------------
pub struct OrIsr {
    children: Vec<Box<dyn Isr>>,
}

impl OrIsr {
    pub fn new(children: Vec<Box<dyn Isr>>) -> Self {
        Self { children }
    }
}

impl Isr for OrIsr {
    fn current_doc(&self) -> u64 {
        self.children.iter().map(|c| c.current_doc()).min().unwrap_or(DONE)
    }

    fn term_freq(&self) -> u32 {
        let doc = self.current_doc();
        self.children.iter()
            .filter(|c| c.current_doc() == doc)
            .map(|c| c.term_freq())
            .sum()
    }

    fn advance(&mut self) {
        let doc = self.current_doc();
        if doc == DONE {
            return;
        }
        for child in &mut self.children {
            if child.current_doc() == doc {
                child.advance();
            }
        }
    }

    fn seek(&mut self, target: u64) {
        for child in &mut self.children {
            child.seek(target);
        }
    }

    fn bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        let doc = self.current_doc();
        self.children.iter()
            .filter(|c| c.current_doc() == doc)
            .map(|c| c.bm25_score(scorer, doc_len))
            .sum()
    }
}

// ---------------------------------------------------------------------------
// NotIsr — subtract a set of documents from a base ISR.
// ---------------------------------------------------------------------------
pub struct NotIsr {
    base:    Box<dyn Isr>,
    exclude: Box<dyn Isr>,
}

impl NotIsr {
    pub fn new(base: Box<dyn Isr>, exclude: Box<dyn Isr>) -> Self {
        let mut s = Self { base, exclude };
        s.skip_excluded();
        s
    }

    fn skip_excluded(&mut self) {
        loop {
            let doc = self.base.current_doc();
            if doc == DONE {
                return;
            }
            self.exclude.seek(doc);
            if self.exclude.current_doc() == doc {
                self.base.advance();
            } else {
                return;
            }
        }
    }
}

impl Isr for NotIsr {
    fn current_doc(&self) -> u64 {
        self.base.current_doc()
    }

    fn term_freq(&self) -> u32 {
        self.base.term_freq()
    }

    fn advance(&mut self) {
        self.base.advance();
        self.skip_excluded();
    }

    fn seek(&mut self, target: u64) {
        self.base.seek(target);
        self.skip_excluded();
    }

    fn bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.base.bm25_score(scorer, doc_len)
    }
}
