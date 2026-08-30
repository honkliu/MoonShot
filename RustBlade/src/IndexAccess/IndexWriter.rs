//! Direct translation of the C++ writer contract; names stay aligned for API parity.
#![allow(non_snake_case, non_upper_case_globals)]

pub trait IndexWriter {
    #[allow(non_snake_case)]
    fn Write(&mut self, tokens: Vec<String>, doc_id: u64, stream: &str);
    #[allow(non_snake_case)]
    fn SetDocImportance(&mut self, doc_id: u64, score: f32);
    #[allow(non_snake_case)]
    fn SetDocVector(&mut self, doc_id: u64, vector: Vec<f32>);
}
