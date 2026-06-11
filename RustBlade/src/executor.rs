use crate::index_reader::IndexReader;
use crate::bm25::Bm25Scorer;
use crate::posting_store::PostingStore;

#[derive(Debug, Clone)]
pub struct SearchResult {
    pub doc_id: u64,
    pub score:  f32,
}

pub struct IndexSearchExecutor<'a> {
    store: &'a PostingStore,
}

impl<'a> IndexSearchExecutor<'a> {
    pub fn new(store: &'a PostingStore) -> Self { Self { store } }

    pub fn execute(&self, reader: &mut dyn IndexReader, top_k: usize) -> Vec<SearchResult> {
        if reader.is_end() { return vec![]; }
        let scorer  = Bm25Scorer::new(self.store.total_docs(), self.store.avg_doc_len());
        let mut results = Vec::new();

        while !reader.is_end() {
            let doc_id  = reader.get_document_id();
            let doc_len = self.store.get_doc_len(doc_id);
            let score   = reader.get_bm25_score(&scorer, doc_len)
                        + self.store.get_doc_importance(doc_id);
            results.push(SearchResult { doc_id, score });
            reader.go_next();
        }

        sort_and_truncate(&mut results, top_k);
        results
    }

    pub fn execute_phased(
        &self,
        phase1:     &mut dyn IndexReader,
        phase2:     &mut dyn IndexReader,
        top_k:      usize,
        min_phase1: usize,
    ) -> Vec<SearchResult> {
        let scorer   = Bm25Scorer::new(self.store.total_docs(), self.store.avg_doc_len());
        let mut results = collect_results(phase1, &scorer, self.store);

        if results.len() < min_phase1 {
            let more = collect_results(phase2, &scorer, self.store);
            merge_results(&mut results, more);
        }

        sort_and_truncate(&mut results, top_k);
        results
    }
}

fn collect_results(r: &mut dyn IndexReader, s: &Bm25Scorer, store: &PostingStore) -> Vec<SearchResult> {
    let mut out = Vec::new();
    while !r.is_end() {
        let doc_id  = r.get_document_id();
        let doc_len = store.get_doc_len(doc_id);
        let score   = r.get_bm25_score(s, doc_len) + store.get_doc_importance(doc_id);
        out.push(SearchResult { doc_id, score });
        r.go_next();
    }
    out
}

fn sort_and_truncate(v: &mut Vec<SearchResult>, top_k: usize) {
    v.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
    if top_k > 0 && v.len() > top_k { v.truncate(top_k); }
}

fn merge_results(base: &mut Vec<SearchResult>, extra: Vec<SearchResult>) {
    for r in extra {
        if let Some(e) = base.iter_mut().find(|e| e.doc_id == r.doc_id) {
            e.score = e.score.max(r.score);
        } else {
            base.push(r);
        }
    }
}
