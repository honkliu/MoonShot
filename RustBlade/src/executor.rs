use crate::index_reader::IndexReader;
use crate::index_reader::ReaderDocumentIDValue;
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

#[allow(non_snake_case)]
impl<'a> IndexSearchExecutor<'a> {
    pub fn new(store: &'a PostingStore) -> Self { Self { store } }

    pub fn Execute(&self, reader: &mut dyn IndexReader, top_k: usize) -> Vec<SearchResult> {
        if reader.IsEnd() { return vec![]; }
        let scorer  = Bm25Scorer::new(self.store.TotalDocs(), self.store.AvgDocLen());
        let mut results = Vec::new();

        while !reader.IsEnd() {
            let doc_id  = reader.GetDocumentID();
            let doc_len = self.store.GetDocLen(ReaderDocumentIDValue(doc_id));
            let score   = reader.GetScore(&scorer, doc_len)
                        + self.store.GetDocImportance(ReaderDocumentIDValue(doc_id));
            results.push(SearchResult { doc_id, score });
            reader.GoNext();
        }

        sort_and_truncate(&mut results, top_k);
        results
    }

    pub fn ExecutePhased(
        &self,
        phase1:     &mut dyn IndexReader,
        phase2:     &mut dyn IndexReader,
        top_k:      usize,
        min_phase1: usize,
    ) -> Vec<SearchResult> {
        let scorer   = Bm25Scorer::new(self.store.TotalDocs(), self.store.AvgDocLen());
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
    while !r.IsEnd() {
        let doc_id  = r.GetDocumentID();
        let doc_id_value = ReaderDocumentIDValue(doc_id);
        let doc_len = store.GetDocLen(doc_id_value);
        let score   = r.GetScore(s, doc_len) + store.GetDocImportance(doc_id_value);
        out.push(SearchResult { doc_id, score });
        r.GoNext();
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
