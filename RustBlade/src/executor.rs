use std::collections::HashMap;
use crate::bm25::Bm25Scorer;
use crate::document::SearchResult;
use crate::inverted_index::InvertedIndex;
use crate::isr::{AndIsr, Isr, NotIsr, OrIsr, DONE};
use crate::query::QueryNode;
use crate::vector_index::VectorIndex;
use crate::fusion::rrf_fusion;

// ---------------------------------------------------------------------------
// QueryExecutor — maps a QueryNode to the correct retrieval path, scores
// results, and merges them.
//
// Mirrors REF's QueryPlanExecutor + RankInstruction chain.
// ---------------------------------------------------------------------------
pub struct QueryExecutor<'a> {
    inv_index:      &'a mut InvertedIndex,
    vec_indexes:    &'a HashMap<String, VectorIndex>,
}

impl<'a> QueryExecutor<'a> {
    pub fn new(
        inv_index:   &'a mut InvertedIndex,
        vec_indexes: &'a HashMap<String, VectorIndex>,
    ) -> Self {
        Self { inv_index, vec_indexes }
    }

    /// Execute `query` and return up to `limit` results sorted by score.
    pub fn execute(&mut self, query: &QueryNode, limit: usize) -> Vec<SearchResult> {
        self.inv_index.reset_isr_pool();
        match query {
            QueryNode::Knn { field, vector, k } => {
                self.execute_knn(field, vector, *k)
            }
            QueryNode::Hybrid { text, vector, field, k, rrf_k } => {
                self.execute_hybrid(text, vector, field, *k, *rrf_k, limit)
            }
            text_query => {
                self.execute_text(text_query, limit)
            }
        }
    }

    // -- text retrieval (BM25 + ISR tree) -----------------------------------

    fn execute_text(&mut self, query: &QueryNode, limit: usize) -> Vec<SearchResult> {
        let scorer = Bm25Scorer::new(self.inv_index.doc_count(), self.inv_index.avg_doc_len());

        let Some(mut isr) = self.build_isr(query) else {
            return vec![];
        };

        let mut results: Vec<SearchResult> = Vec::new();

        while !isr.is_done() {
            let doc_id  = isr.current_doc();
            let doc_len = self.inv_index.doc_len(doc_id);
            let score   = isr.bm25_score(&scorer, doc_len);

            results.push(SearchResult::new(doc_id, score));
            isr.advance();
        }

        results.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
        results.truncate(limit);
        results
    }

    // -- vector (KNN) retrieval ----------------------------------------------

    fn execute_knn(&self, field: &str, vector: &[f32], k: usize) -> Vec<SearchResult> {
        let Some(idx) = self.vec_indexes.get(field) else {
            return vec![];
        };
        let ef = (k * 4).max(50);
        idx.search(vector, k, ef)
            .into_iter()
            .map(|(id, score)| SearchResult::new(id, score))
            .collect()
    }

    // -- hybrid (text + vector fused with RRF) -------------------------------

    fn execute_hybrid(
        &mut self,
        text:    &QueryNode,
        vector:  &[f32],
        field:   &str,
        k:       usize,
        rrf_k:   f32,
        limit:   usize,
    ) -> Vec<SearchResult> {
        let text_top = limit.max(k) * 4;   // retrieve more than needed for fusion
        let text_hits = self.execute_text(text, text_top);
        let vec_hits  = self.execute_knn(field, vector, k * 4);

        let text_ranking: Vec<(u64, f32)> = text_hits.iter().map(|r| (r.doc_id, r.score)).collect();
        let vec_ranking:  Vec<(u64, f32)> = vec_hits.iter().map(|r| (r.doc_id, r.score)).collect();

        rrf_fusion(vec![text_ranking, vec_ranking], rrf_k, limit)
            .into_iter()
            .map(|(id, score)| SearchResult::new(id, score))
            .collect()
    }

    // -- ISR construction ---------------------------------------------------

    /// Recursively build the ISR tree for a text QueryNode.
    fn build_isr(&mut self, node: &QueryNode) -> Option<Box<dyn Isr>> {
        match node {
            QueryNode::Term(term) => {
                self.inv_index.open_isr(term)
            }
            QueryNode::And(children) => {
                let isrs: Vec<Box<dyn Isr>> = children.iter()
                    .filter_map(|c| self.build_isr(c))
                    .collect();
                match isrs.len() {
                    0 => None,
                    1 => Some(isrs.into_iter().next().unwrap()),
                    _ => Some(Box::new(AndIsr::new(isrs))),
                }
            }
            QueryNode::Or(children) => {
                let isrs: Vec<Box<dyn Isr>> = children.iter()
                    .filter_map(|c| self.build_isr(c))
                    .collect();
                match isrs.len() {
                    0 => None,
                    1 => Some(isrs.into_iter().next().unwrap()),
                    _ => Some(Box::new(OrIsr::new(isrs))),
                }
            }
            QueryNode::Not { base, exclude } => {
                let base_isr    = self.build_isr(base)?;
                let exclude_isr = self.build_isr(exclude)?;
                Some(Box::new(NotIsr::new(base_isr, exclude_isr)))
            }
            // Vector nodes are handled in execute(), not here.
            _ => None,
        }
    }
}
