//! Direct counterpart of the C++ search result while retaining idiomatic Rust field access.
#![allow(non_snake_case, non_upper_case_globals)]

#[derive(Debug, Clone, Default)]
pub struct SearchResult {
    pub doc_id: u64,
    pub score: f32,
    pub snippet: String,
}
