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
            if self.children.iter().any(|c| c.is_end()) { return; }

            let pivot = self.children.iter()
                .map(|c| c.get_document_id())
                .max()
                .unwrap_or(NO_MORE_DOCS);

            let mut aligned = true;
            for c in &mut self.children {
                if c.get_document_id() != pivot {
                    c.go_until(pivot);
                    if c.is_end()                    { return; }
                    if c.get_document_id() != pivot  { aligned = false; break; }
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
    fn go_next(&mut self) {
        if self.is_end() { return; }
        let doc = self.get_document_id();
        for c in &mut self.children {
            if !c.is_end() && c.get_document_id() == doc { c.go_next(); }
        }
        self.align_to_pivot();
    }

    fn go_until(&mut self, target: u64) {
        for c in &mut self.children { c.go_until(target); }
        self.align_to_pivot();
    }

    fn is_end(&self) -> bool {
        self.children.is_empty() || self.children.iter().any(|c| c.is_end())
    }

    fn get_document_id(&self) -> u64 {
        if self.is_end() { NO_MORE_DOCS } else { self.children[0].get_document_id() }
    }

    fn get_term_freq(&self) -> u32 {
        self.children.iter().map(|c| c.get_term_freq()).sum()
    }

    fn get_bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.children.iter().map(|c| c.get_bm25_score(scorer, doc_len)).sum()
    }

    fn set_debug(&mut self, label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        println!("{}[AND]", " ".repeat(depth * 2));
        for c in &mut self.children { c.set_debug(label, depth + 1); }
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
    fn go_next(&mut self) {
        if self.active_count == 0 { return; }
        let doc = self.get_document_id();
        let mut i = 0;
        while i < self.active_count {
            if self.children[i].get_document_id() == doc {
                self.children[i].go_next();
                if self.children[i].is_end() {
                    self.active_count -= 1;
                    self.children.swap(i, self.active_count);
                    continue;
                }
            }
            i += 1;
        }
    }

    fn go_until(&mut self, target: u64) {
        let mut i = 0;
        while i < self.active_count {
            if self.children[i].get_document_id() < target {
                self.children[i].go_until(target);
                if self.children[i].is_end() {
                    self.active_count -= 1;
                    self.children.swap(i, self.active_count);
                    continue;
                }
            }
            i += 1;
        }
    }

    fn is_end(&self) -> bool { self.active_count == 0 }

    fn get_document_id(&self) -> u64 {
        (0..self.active_count)
            .map(|i| self.children[i].get_document_id())
            .min()
            .unwrap_or(NO_MORE_DOCS)
    }

    fn get_term_freq(&self) -> u32 {
        let doc = self.get_document_id();
        (0..self.active_count)
            .filter(|&i| self.children[i].get_document_id() == doc)
            .map(|i| self.children[i].get_term_freq())
            .sum()
    }

    fn get_bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        let doc = self.get_document_id();
        (0..self.active_count)
            .filter(|&i| self.children[i].get_document_id() == doc)
            .map(|i| self.children[i].get_bm25_score(scorer, doc_len))
            .sum()
    }

    fn set_debug(&mut self, label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        println!("{}[OR]", " ".repeat(depth * 2));
        for c in &mut self.children { c.set_debug(label, depth + 1); }
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
        while !self.base.is_end() {
            let doc = self.base.get_document_id();
            self.exclude.go_until(doc);
            if !self.exclude.is_end() && self.exclude.get_document_id() == doc {
                if self.debug {
                    let ind = " ".repeat(self.debug_depth * 2);
                    println!("{}NOT excluded  doc {}", ind, doc);
                }
                self.base.go_next();
            } else {
                break;
            }
        }
    }
}

impl IndexReader for NotIndexReader {
    fn go_next(&mut self)             { self.base.go_next(); self.skip_excluded(); }
    fn go_until(&mut self, t: u64)    { self.base.go_until(t); self.skip_excluded(); }
    fn is_end(&self)                  -> bool  { self.base.is_end() }
    fn get_document_id(&self)         -> u64   { self.base.get_document_id() }
    fn get_term_freq(&self)           -> u32   { self.base.get_term_freq() }

    fn get_bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.base.get_bm25_score(scorer, doc_len)
    }

    fn set_debug(&mut self, label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        let ind = " ".repeat(depth * 2);
        println!("{}[NOT]", ind);
        println!("{}  + base:", ind);
        self.base.set_debug(label, depth + 2);
        println!("{}  - excl:", ind);
        self.exclude.set_debug(label, depth + 2);
    }
}
