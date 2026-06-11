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
* Composites aggregate from leaves via get_bm25_score / get_term_freq.
*
* Mirrors MoonShot's IndexReader.h and REF's ISR hierarchy.
*/
pub trait IndexReader {
    fn go_next(&mut self);
    fn go_until(&mut self, target: u64);
    fn is_end(&self)            -> bool;
    fn get_document_id(&self)   -> u64;
    fn get_term_freq(&self)     -> u32   { 1 }
    fn get_bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 { 0.0 }
    fn set_debug(&mut self, _label: &str, _depth: usize) {}
    fn close(&mut self) {}
}
