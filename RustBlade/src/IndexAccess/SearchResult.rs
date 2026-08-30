#[derive(Debug, Clone, Default)]
pub struct SearchResult {
    pub doc_id: u64,
    pub score: f32,
    pub snippet: String,
}
