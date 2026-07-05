use crate::index_reader::IndexReader;
use crate::index_reader::ReaderDocumentIDValue;
use crate::bm25::Bm25Scorer;
use crate::block_table::{DOC_REC_SIZE, DOC_VECTOR_DIM, DOC_VECTOR_OFFSET};
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
        self.ExecuteWithVector(reader, top_k, &[], 0, None)
    }

    pub fn ExecuteWithVector(
        &self,
        reader: &mut dyn IndexReader,
        top_k: usize,
        docdata: &[u8],
        docdata_first_doc_id: u64,
        vector_query: Option<&[f32]>,
    ) -> Vec<SearchResult> {
        if reader.IsEnd() { return vec![]; }
        let scorer  = Bm25Scorer::new(self.store.TotalDocs(), self.store.AvgDocLen());
        let mut results = Vec::new();

        while !reader.IsEnd() {
            let doc_id  = reader.GetDocumentID();
            let doc_id_value = ReaderDocumentIDValue(doc_id);
            let score   = reader.GetScore(&scorer, self.store, doc_id_value)
                        + self.store.GetDocImportance(doc_id_value)
                        + vector_score_feature(docdata, docdata_first_doc_id, doc_id_value, vector_query);
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

fn vector_score_feature(docdata: &[u8], first_doc_id: u64, doc_id: u64, query: Option<&[f32]>) -> f32 {
    let Some(query) = query else { return 0.0; };
    if query.len() != DOC_VECTOR_DIM || doc_id < first_doc_id { return 0.0; }
    let slot = (doc_id - first_doc_id) as usize;
    let offset = slot.saturating_mul(DOC_REC_SIZE);
    if offset + DOC_REC_SIZE > docdata.len() { return 0.0; }
    let stored_doc_id = u32::from_le_bytes(docdata[offset..offset + 4].try_into().unwrap()) as u64;
    let vector_dim = u16::from_le_bytes(docdata[offset + 54..offset + 56].try_into().unwrap()) as usize;
    let vector_format = u16::from_le_bytes(docdata[offset + 56..offset + 58].try_into().unwrap());
    if stored_doc_id != doc_id || vector_dim != DOC_VECTOR_DIM || vector_format == 0 { return 0.0; }

    let mut dot = 0.0f32;
    let mut nq = 0.0f32;
    let mut nd = 0.0f32;
    for i in 0..DOC_VECTOR_DIM {
        let q = query[i];
        let d = docdata[offset + DOC_VECTOR_OFFSET + i] as i8 as f32 / 128.0;
        dot += q * d;
        nq += q * q;
        nd += d * d;
    }
    if nq <= 0.0 || nd <= 0.0 { return 0.0; }
    128.0 * dot / (nq.sqrt() * nd.sqrt())
}

fn collect_results(r: &mut dyn IndexReader, s: &Bm25Scorer, store: &PostingStore) -> Vec<SearchResult> {
    let mut out = Vec::new();
    while !r.IsEnd() {
        let doc_id  = r.GetDocumentID();
        let doc_id_value = ReaderDocumentIDValue(doc_id);
        let score   = r.GetScore(s, store, doc_id_value) + store.GetDocImportance(doc_id_value);
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
