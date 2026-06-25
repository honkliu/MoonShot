use crate::bm25::Bm25Scorer;

pub const NO_MORE_DOCS: u64 = u64::MAX;
pub const READER_DOCID_SOURCE_SHIFT: u64 = 59;
pub const READER_DOCID_VALUE_MASK: u64 = (1u64 << READER_DOCID_SOURCE_SHIFT) - 1;
pub const READER_SOURCE_ANCHOR: u8 = 1u8 << 0;
pub const READER_SOURCE_URL: u8 = 1u8 << 1;
pub const READER_SOURCE_TITLE: u8 = 1u8 << 2;
pub const READER_SOURCE_BODY: u8 = 1u8 << 3;
pub const READER_SOURCE_VECTOR: u8 = 1u8 << 4;

#[allow(non_snake_case)]
pub fn ReaderDocumentIDValue(docId: u64) -> u64 {
    docId & READER_DOCID_VALUE_MASK
}

#[allow(non_snake_case)]
pub fn ReaderDocumentIDSourceMask(docId: u64) -> u8 {
    (docId >> READER_DOCID_SOURCE_SHIFT) as u8
}

#[allow(non_snake_case)]
pub fn MakeReaderDocumentID(docId: u64, sourceMask: u8) -> u64 {
    ((sourceMask as u64) << READER_DOCID_SOURCE_SHIFT) | (docId & READER_DOCID_VALUE_MASK)
}

#[allow(non_snake_case)]
pub fn ReaderSourceMaskForStream(stream: char) -> u8 {
    match stream {
        'A' => READER_SOURCE_ANCHOR,
        'U' => READER_SOURCE_URL,
        'T' => READER_SOURCE_TITLE,
        'B' => READER_SOURCE_BODY,
        'V' => READER_SOURCE_VECTOR,
        _ => 0,
    }
}

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
    fn GoUntil(&mut self, target: u64, limit: u64);
    fn IsEnd(&self)            -> bool;
    fn GetDocumentID(&self)   -> u64;
    fn GetTermFreq(&self)     -> u32   { 1 }
    fn GetScore(&self, _scorer: &Bm25Scorer, _doc_len: u32) -> f32 { 0.0 }
    fn GetSourceMask(&self) -> u8 { 0 }
    fn SetDebug(&mut self, _label: &str, _depth: usize) {}
    fn Close(&mut self) {}
}
