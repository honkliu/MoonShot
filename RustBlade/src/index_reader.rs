use crate::bm25::Bm25Scorer;

pub const NO_MORE_DOCS: u64 = u64::MAX;

/*
* IndexReader — the single interface for traversing posting lists.
*
* Leaf:      AdvancedIndexReader — reads one (term+stream) posting list
*            from IndexBlockTable via VarByteDecoder.
* Composite: AndIndexReader / OrIndexReader / NotIndexReader — combine leaves.
*
* TF and BM25 score live ONLY on leaf readers.
* Composites aggregate from leaves via GetScore / GetTermFreq.
*
* Mirrors MoonShot's IndexReader.h and REF's ISR hierarchy.
*/
#[allow(non_snake_case)]
pub trait IndexReader {
    fn GoNext(&mut self);
    fn GoUntil(&mut self, target: u64);
    fn IsEnd(&self)            -> bool;
    fn GetDocumentID(&self)   -> u64;
    fn GetTermFreq(&self)     -> u32   { 1 }
    fn GetScore(&self, _scorer: &Bm25Scorer, _doc_len: u32) -> f32 { 0.0 }
    fn SetDebug(&mut self, _label: &str, _depth: usize) {}
    fn Close(&mut self) {}
}
