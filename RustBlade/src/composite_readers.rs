use crate::index_reader::{IndexReader, NO_MORE_DOCS};
use crate::bm25::Bm25Scorer;

/* ------------------------------------------------------------------ */

/*
* AndIndexReader — DAAT intersection: all children must land on the same doc.
* AlignToPivot drives all children to the maximum current doc ID,
* restarting until all agree or any child exhausts.
*/
pub struct AndIndexReader {
    children:    Vec<Box<dyn IndexReader>>,
    debug:       bool,
    debug_depth: usize,
}

impl AndIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>) -> Self {
        let mut r = Self { children, debug: false, debug_depth: 0 };
        r.align_to_pivot();
        r
    }

    fn align_to_pivot(&mut self) {
        loop {
            if self.children.iter().any(|c| c.IsEnd()) { return; }

            let pivot = self.children.iter()
                .map(|c| c.GetDocumentID())
                .max()
                .unwrap_or(NO_MORE_DOCS);

            let mut aligned = true;
            for c in &mut self.children {
                if c.GetDocumentID() != pivot {
                    c.GoUntil(pivot);
                    if c.IsEnd()                    { return; }
                    if c.GetDocumentID() != pivot  { aligned = false; break; }
                }
            }

            if aligned {
                if self.debug {
                    let ind = " ".repeat(self.debug_depth * 2);
                    println!("{}AND match  doc {}", ind, pivot);
                }
                return;
            }
        }
    }
}

impl IndexReader for AndIndexReader {
    fn GoNext(&mut self) {
        if self.IsEnd() { return; }
        let doc = self.GetDocumentID();
        for c in &mut self.children {
            if !c.IsEnd() && c.GetDocumentID() == doc { c.GoNext(); }
        }
        self.align_to_pivot();
    }

    fn GoUntil(&mut self, target: u64) {
        for c in &mut self.children { c.GoUntil(target); }
        self.align_to_pivot();
    }

    fn IsEnd(&self) -> bool {
        self.children.is_empty() || self.children.iter().any(|c| c.IsEnd())
    }

    fn GetDocumentID(&self) -> u64 {
        if self.IsEnd() { NO_MORE_DOCS } else { self.children[0].GetDocumentID() }
    }

    fn GetTermFreq(&self) -> u32 {
        self.children.iter().map(|c| c.GetTermFreq()).sum()
    }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.children.iter().map(|c| c.GetScore(scorer, doc_len)).sum()
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        println!("{}[AND]", " ".repeat(depth * 2));
        for c in &mut self.children { c.SetDebug(label, depth + 1); }
    }
}

/* ------------------------------------------------------------------ */

/*
* OrIndexReader — DAAT union with shrinking active set.
*
* active_count tracks live (non-exhausted) readers in children[0..active_count).
* When a reader exhausts it is swapped to children[active_count..] and never
* visited again — O(active) per step, shrinking over time.
*/
pub struct OrIndexReader {
    children:    Vec<Box<dyn IndexReader>>,
    active_count: usize,
    debug:        bool,
    debug_depth:  usize,
}

impl OrIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>) -> Self {
        let active_count = children.len();
        Self { children, active_count, debug: false, debug_depth: 0 }
    }
}

impl IndexReader for OrIndexReader {
    fn GoNext(&mut self) {
        if self.active_count == 0 { return; }
        let doc = self.GetDocumentID();
        let mut i = 0;
        while i < self.active_count {
            if self.children[i].GetDocumentID() == doc {
                self.children[i].GoNext();
                if self.children[i].IsEnd() {
                    self.active_count -= 1;
                    self.children.swap(i, self.active_count);
                    continue;
                }
            }
            i += 1;
        }
    }

    fn GoUntil(&mut self, target: u64) {
        let mut i = 0;
        while i < self.active_count {
            if self.children[i].GetDocumentID() < target {
                self.children[i].GoUntil(target);
                if self.children[i].IsEnd() {
                    self.active_count -= 1;
                    self.children.swap(i, self.active_count);
                    continue;
                }
            }
            i += 1;
        }
    }

    fn IsEnd(&self) -> bool { self.active_count == 0 }

    fn GetDocumentID(&self) -> u64 {
        (0..self.active_count)
            .map(|i| self.children[i].GetDocumentID())
            .min()
            .unwrap_or(NO_MORE_DOCS)
    }

    fn GetTermFreq(&self) -> u32 {
        let doc = self.GetDocumentID();
        (0..self.active_count)
            .filter(|&i| self.children[i].GetDocumentID() == doc)
            .map(|i| self.children[i].GetTermFreq())
            .sum()
    }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        let doc = self.GetDocumentID();
        (0..self.active_count)
            .filter(|&i| self.children[i].GetDocumentID() == doc)
            .map(|i| self.children[i].GetScore(scorer, doc_len))
            .sum()
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        println!("{}[OR]", " ".repeat(depth * 2));
        for c in &mut self.children { c.SetDebug(label, depth + 1); }
    }
}

/* ------------------------------------------------------------------ */

/*
* NotIndexReader — base reader filtered by an exclusion reader.
* SkipExcluded advances base past any doc also present in exclude.
*/
pub struct NotIndexReader {
    base:        Box<dyn IndexReader>,
    exclude:     Box<dyn IndexReader>,
    debug:       bool,
    debug_depth: usize,
}

impl NotIndexReader {
    pub fn new(base: Box<dyn IndexReader>, exclude: Box<dyn IndexReader>) -> Self {
        let mut r = Self { base, exclude, debug: false, debug_depth: 0 };
        r.skip_excluded();
        r
    }

    fn skip_excluded(&mut self) {
        while !self.base.IsEnd() {
            let doc = self.base.GetDocumentID();
            self.exclude.GoUntil(doc);
            if !self.exclude.IsEnd() && self.exclude.GetDocumentID() == doc {
                if self.debug {
                    let ind = " ".repeat(self.debug_depth * 2);
                    println!("{}NOT excluded  doc {}", ind, doc);
                }
                self.base.GoNext();
            } else {
                break;
            }
        }
    }
}

impl IndexReader for NotIndexReader {
    fn GoNext(&mut self)             { self.base.GoNext(); self.skip_excluded(); }
    fn GoUntil(&mut self, t: u64)    { self.base.GoUntil(t); self.skip_excluded(); }
    fn IsEnd(&self)                  -> bool  { self.base.IsEnd() }
    fn GetDocumentID(&self)         -> u64   { self.base.GetDocumentID() }
    fn GetTermFreq(&self)           -> u32   { self.base.GetTermFreq() }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.base.GetScore(scorer, doc_len)
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        let ind = " ".repeat(depth * 2);
        println!("{}[NOT]", ind);
        println!("{}  + base:", ind);
        self.base.SetDebug(label, depth + 2);
        println!("{}  - excl:", ind);
        self.exclude.SetDebug(label, depth + 2);
    }
}
