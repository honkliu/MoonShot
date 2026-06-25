use crate::index_reader::{IndexReader, NO_MORE_DOCS, READER_SOURCE_VECTOR};
use crate::bm25::Bm25Scorer;
use crate::vector_index::VectorSearchResult;

/* ------------------------------------------------------------------ */

/*
* AndIndexReader — DAAT intersection: all children must land on the same doc.
* AlignToPivot drives all children to the maximum current doc ID,
* restarting until all agree or any child exhausts.
*/
#[allow(non_snake_case)]
pub struct AndIndexReader {
    m_Children:    Vec<Box<dyn IndexReader>>,
    m_Debug:       bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl AndIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>) -> Self {
        let mut r = Self { m_Children: children, m_Debug: false, m_DebugDepth: 0 };
        r.AlignToPivot();
        r
    }

    fn AlignToPivot(&mut self) {
        loop {
            if self.m_Children.iter().any(|c| c.IsEnd()) { return; }

            let pivot = self.m_Children.iter()
                .map(|c| c.GetDocumentID())
                .max()
                .unwrap_or(NO_MORE_DOCS);

            let mut aligned = true;
            for c in &mut self.m_Children {
                if c.GetDocumentID() != pivot {
                    c.GoUntil(pivot, NO_MORE_DOCS);
                    if c.IsEnd()                    { return; }
                    if c.GetDocumentID() != pivot  { aligned = false; break; }
                }
            }

            if aligned {
                if self.m_Debug {
                    let ind = " ".repeat(self.m_DebugDepth * 2);
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
        for c in &mut self.m_Children {
            if !c.IsEnd() && c.GetDocumentID() == doc { c.GoNext(); }
        }
        self.AlignToPivot();
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        for c in &mut self.m_Children { c.GoUntil(target, limit); }
        self.AlignToPivot();
    }

    fn IsEnd(&self) -> bool {
        self.m_Children.is_empty() || self.m_Children.iter().any(|c| c.IsEnd())
    }

    fn GetDocumentID(&self) -> u64 {
        if self.IsEnd() { NO_MORE_DOCS } else { self.m_Children[0].GetDocumentID() }
    }

    fn GetTermFreq(&self) -> u32 {
        self.m_Children.iter().map(|c| c.GetTermFreq()).sum()
    }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.m_Children.iter().map(|c| c.GetScore(scorer, doc_len)).sum()
    }

    fn GetSourceMask(&self) -> u8 {
        let doc = self.GetDocumentID();
        self.m_Children.iter()
            .filter(|c| !c.IsEnd() && c.GetDocumentID() == doc)
            .fold(0u8, |mask, c| mask | c.GetSourceMask())
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug       = true;
        self.m_DebugDepth = depth;
        println!("{}[AND]", " ".repeat(depth * 2));
        for c in &mut self.m_Children { c.SetDebug(label, depth + 1); }
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
#[allow(non_snake_case)]
pub struct OrIndexReader {
    m_Children:    Vec<Box<dyn IndexReader>>,
    m_Debug:        bool,
    m_DebugDepth:  usize,
}

#[allow(non_snake_case)]
impl OrIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>) -> Self {
        Self { m_Children: children, m_Debug: false, m_DebugDepth: 0 }
    }
}

impl IndexReader for OrIndexReader {
    fn GoNext(&mut self) {
        if self.IsEnd() { return; }
        let doc = self.GetDocumentID();
        for child in &mut self.m_Children {
            if !child.IsEnd() && child.GetDocumentID() == doc {
                child.GoNext();
            }
        }
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        for child in &mut self.m_Children {
            if !child.IsEnd() && child.GetDocumentID() < target && child.GetDocumentID() < limit {
                child.GoUntil(target, limit);
            }
        }
    }

    fn IsEnd(&self) -> bool {
        self.m_Children.iter().all(|child| child.IsEnd())
    }

    fn GetDocumentID(&self) -> u64 {
        self.m_Children.iter()
            .filter(|child| !child.IsEnd())
            .map(|child| child.GetDocumentID())
            .min()
            .unwrap_or(NO_MORE_DOCS)
    }

    fn GetTermFreq(&self) -> u32 {
        let doc = self.GetDocumentID();
        self.m_Children.iter()
            .filter(|child| !child.IsEnd() && child.GetDocumentID() == doc)
            .map(|child| child.GetTermFreq())
            .sum()
    }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        let doc = self.GetDocumentID();
        self.m_Children.iter()
            .filter(|child| !child.IsEnd() && child.GetDocumentID() == doc)
            .map(|child| child.GetScore(scorer, doc_len))
            .sum()
    }

    fn GetSourceMask(&self) -> u8 {
        let doc = self.GetDocumentID();
        self.m_Children.iter()
            .filter(|child| !child.IsEnd() && child.GetDocumentID() == doc)
            .fold(0u8, |mask, child| mask | child.GetSourceMask())
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug       = true;
        self.m_DebugDepth = depth;
        println!("{}[OR]", " ".repeat(depth * 2));
        for c in &mut self.m_Children { c.SetDebug(label, depth + 1); }
    }
}

/* ------------------------------------------------------------------ */

/*
* NotIndexReader — base reader filtered by an exclusion reader.
* SkipExcluded advances base past any doc also present in exclude.
*/
#[allow(non_snake_case)]
pub struct NotIndexReader {
    m_Base:        Box<dyn IndexReader>,
    m_Exclude:     Box<dyn IndexReader>,
    m_Debug:       bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl NotIndexReader {
    pub fn new(base: Box<dyn IndexReader>, exclude: Box<dyn IndexReader>) -> Self {
        let mut r = Self { m_Base: base, m_Exclude: exclude, m_Debug: false, m_DebugDepth: 0 };
        r.SkipExcluded();
        r
    }

    fn SkipExcluded(&mut self) {
        while !self.m_Base.IsEnd() {
            let doc = self.m_Base.GetDocumentID();
            self.m_Exclude.GoUntil(doc, NO_MORE_DOCS);
            if !self.m_Exclude.IsEnd() && self.m_Exclude.GetDocumentID() == doc {
                if self.m_Debug {
                    let ind = " ".repeat(self.m_DebugDepth * 2);
                    println!("{}NOT excluded  doc {}", ind, doc);
                }
                self.m_Base.GoNext();
            } else {
                break;
            }
        }
    }
}

impl IndexReader for NotIndexReader {
    fn GoNext(&mut self)             { self.m_Base.GoNext(); self.SkipExcluded(); }
    fn GoUntil(&mut self, target: u64, limit: u64)    { self.m_Base.GoUntil(target, limit); self.SkipExcluded(); }
    fn IsEnd(&self)                  -> bool  { self.m_Base.IsEnd() }
    fn GetDocumentID(&self)         -> u64   { self.m_Base.GetDocumentID() }
    fn GetTermFreq(&self)           -> u32   { self.m_Base.GetTermFreq() }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        self.m_Base.GetScore(scorer, doc_len)
    }

    fn GetSourceMask(&self) -> u8 { self.m_Base.GetSourceMask() }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug       = true;
        self.m_DebugDepth = depth;
        let ind = " ".repeat(depth * 2);
        println!("{}[NOT]", ind);
        println!("{}  + base:", ind);
        self.m_Base.SetDebug(label, depth + 2);
        println!("{}  - excl:", ind);
        self.m_Exclude.SetDebug(label, depth + 2);
    }
}

#[allow(non_snake_case)]
pub struct VectorIndexReader {
    m_Results: Vec<VectorSearchResult>,
    m_Pos: usize,
}

#[allow(non_snake_case)]
impl VectorIndexReader {
    pub fn new(mut results: Vec<VectorSearchResult>) -> Self {
        results.sort_by(|a, b| a.doc_id.cmp(&b.doc_id));
        Self { m_Results: results, m_Pos: 0 }
    }
}

impl IndexReader for VectorIndexReader {
    fn GoNext(&mut self) {
        if !self.IsEnd() { self.m_Pos += 1; }
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        while !self.IsEnd() && self.GetDocumentID() < target && self.GetDocumentID() < limit {
            self.m_Pos += 1;
        }
    }

    fn IsEnd(&self) -> bool { self.m_Pos >= self.m_Results.len() }

    fn GetDocumentID(&self) -> u64 {
        if self.IsEnd() { NO_MORE_DOCS } else { self.m_Results[self.m_Pos].doc_id }
    }

    fn GetScore(&self, _scorer: &Bm25Scorer, _doc_len: u32) -> f32 {
        if self.IsEnd() { 0.0 } else { self.m_Results[self.m_Pos].score }
    }

    fn GetSourceMask(&self) -> u8 { if self.IsEnd() { 0 } else { READER_SOURCE_VECTOR } }

    fn Close(&mut self) { self.m_Pos = self.m_Results.len(); }
}
