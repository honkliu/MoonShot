//! Direct translation of the C++ search executor; symbol names stay aligned for debugging.
#![allow(non_snake_case, non_upper_case_globals)]

use std::cmp::{Ordering, Reverse};
use std::collections::BinaryHeap;
use std::sync::{OnceLock, RwLock};

use crate::block_table::{DocDataDecodeScore, DocDataEntry, DOC_VECTOR_DIM};
use crate::eval_expression::{kWeakAndBigramParameters, QueryCompileModeParameters};
use crate::index_reader::{IndexReader, MakeReaderDocumentID};
use crate::search_result::SearchResult;

pub trait SearchExecutionContext {
    fn GetDocDataEntry(&self, docId: u64) -> Option<&DocDataEntry>;
}

#[derive(Clone, Copy, PartialEq)]
struct ScoreKey(f32);
impl Eq for ScoreKey {}
impl PartialOrd for ScoreKey {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for ScoreKey {
    fn cmp(&self, other: &Self) -> Ordering {
        self.0.partial_cmp(&other.0).unwrap_or(Ordering::Equal)
    }
}

struct HeapEntry {
    score: ScoreKey,
    result: SearchResult,
}
impl PartialEq for HeapEntry {
    fn eq(&self, other: &Self) -> bool {
        self.score == other.score
    }
}
impl Eq for HeapEntry {}
impl PartialOrd for HeapEntry {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for HeapEntry {
    fn cmp(&self, other: &Self) -> Ordering {
        self.score.cmp(&other.score)
    }
}

pub struct IndexSearchExecutor<'a> {
    m_Context: &'a dyn SearchExecutionContext,
}

#[allow(non_snake_case)]
impl<'a> IndexSearchExecutor<'a> {
    pub fn new(context: &'a dyn SearchExecutionContext) -> Self {
        Self { m_Context: context }
    }

    pub fn SetScoringParameters(parameters: QueryCompileModeParameters) {
        *ScoringParameters().write().unwrap() = parameters;
    }

    pub fn Execute(&self, reader: &mut dyn IndexReader, topK: i32) -> Vec<SearchResult> {
        self.ExecuteWithVector(reader, topK, None)
    }

    pub fn ExecuteWithVector(
        &self,
        reader: &mut dyn IndexReader,
        topK: i32,
        vectorQuery: Option<&[f32]>,
    ) -> Vec<SearchResult> {
        if reader.IsEnd() {
            return Vec::new();
        }

        let limit = Self::TopKLimit(topK);
        let parameters = *ScoringParameters().read().unwrap();
        let mut results = Vec::new();
        let mut heap: BinaryHeap<Reverse<HeapEntry>> = BinaryHeap::new();
        while !reader.IsEnd() {
            let docId = reader.GetDocumentID();
            let entry = self
                .m_Context
                .GetDocDataEntry(docId)
                .expect("IndexReader returned a document without DocDataEntry");
            let score = reader.GetScore(entry)
                + DocDataScore(entry, &parameters)
                + VectorScoreFeature(entry, vectorQuery, &parameters);
            let result = SearchResult {
                doc_id: MakeReaderDocumentID(docId, reader.GetSourceMask()),
                score,
                snippet: String::new(),
            };

            if limit.is_none() {
                results.push(result);
            } else if heap.len() < limit.unwrap() {
                heap.push(Reverse(HeapEntry {
                    score: ScoreKey(score),
                    result,
                }));
            } else if score > heap.peek().unwrap().0.result.score {
                heap.pop();
                heap.push(Reverse(HeapEntry {
                    score: ScoreKey(score),
                    result,
                }));
            }

            reader.GoNext();
        }

        if limit.is_some() {
            results.extend(heap.into_iter().map(|Reverse(entry)| entry.result));
        }
        Self::SortAndTruncate(&mut results, limit);
        results
    }

    pub fn ExecuteBounded(
        &self,
        reader: &mut dyn IndexReader,
        topK: i32,
        maxVisitedDocs: u64,
        vectorQuery: Option<&[f32]>,
    ) -> Vec<SearchResult> {
        if reader.IsEnd() || maxVisitedDocs == 0 {
            return Vec::new();
        }

        let limit = Self::TopKLimit(topK);
        let parameters = *ScoringParameters().read().unwrap();
        let mut results = Vec::new();
        let mut heap: BinaryHeap<Reverse<HeapEntry>> = BinaryHeap::new();
        let mut visited = 0u64;
        while !reader.IsEnd() && visited < maxVisitedDocs {
            let docId = reader.GetDocumentID();
            let entry = self
                .m_Context
                .GetDocDataEntry(docId)
                .expect("IndexReader returned a document without DocDataEntry");
            let score = reader.GetScore(entry)
                + DocDataScore(entry, &parameters)
                + VectorScoreFeature(entry, vectorQuery, &parameters);
            let result = SearchResult {
                doc_id: MakeReaderDocumentID(docId, reader.GetSourceMask()),
                score,
                snippet: String::new(),
            };

            if limit.is_none() {
                results.push(result);
            } else if heap.len() < limit.unwrap() {
                heap.push(Reverse(HeapEntry {
                    score: ScoreKey(score),
                    result,
                }));
            } else if score > heap.peek().unwrap().0.result.score {
                heap.pop();
                heap.push(Reverse(HeapEntry {
                    score: ScoreKey(score),
                    result,
                }));
            }

            visited += 1;
            reader.GoNext();
        }

        if limit.is_some() {
            results.extend(heap.into_iter().map(|Reverse(entry)| entry.result));
        }
        Self::SortAndTruncate(&mut results, limit);
        results
    }

    fn TopKLimit(topK: i32) -> Option<usize> {
        usize::try_from(topK).ok().filter(|limit| *limit > 0)
    }

    fn SortAndTruncate(results: &mut Vec<SearchResult>, limit: Option<usize>) {
        results.sort_by(|a, b| {
            if a.score > b.score {
                Ordering::Less
            } else if b.score > a.score {
                Ordering::Greater
            } else {
                Ordering::Equal
            }
        });
        if let Some(limit) = limit {
            if results.len() > limit {
                results.truncate(limit);
            }
        }
    }
}

#[allow(non_snake_case)]
fn ScoringParameters() -> &'static RwLock<QueryCompileModeParameters> {
    static PARAMETERS: OnceLock<RwLock<QueryCompileModeParameters>> = OnceLock::new();
    PARAMETERS.get_or_init(|| RwLock::new(kWeakAndBigramParameters))
}

#[allow(non_snake_case)]
fn DocDataPrior(entry: &DocDataEntry) -> f32 {
    let docLength = entry.DDE_BodyLength.max(1) as f32;
    let lengthQuality = (1.0 - (docLength.log2() - 6.0).abs() / 4.0).max(0.0);
    0.15 * lengthQuality
        + 0.10 * DocDataDecodeScore(entry.DDE_QualityScore)
        + 0.05 * DocDataDecodeScore(entry.DDE_AuthorityScore)
        - 0.10 * DocDataDecodeScore(entry.DDE_SpamScore)
}

#[allow(non_snake_case)]
fn DocDataScore(entry: &DocDataEntry, parameters: &QueryCompileModeParameters) -> f32 {
    parameters.QMP_StaticWeight * DocDataDecodeScore(entry.DDE_StaticRank)
        + parameters.QMP_PriorWeight * DocDataPrior(entry)
        + parameters.QMP_QualityWeight * DocDataDecodeScore(entry.DDE_QualityScore)
        + parameters.QMP_AuthorityWeight * DocDataDecodeScore(entry.DDE_AuthorityScore)
        - parameters.QMP_SpamPenalty * DocDataDecodeScore(entry.DDE_SpamScore)
}

#[allow(non_snake_case)]
fn VectorScoreFeature(
    entry: &DocDataEntry,
    query: Option<&[f32]>,
    parameters: &QueryCompileModeParameters,
) -> f32 {
    let Some(query) = query else {
        return 0.0;
    };
    if query.len() != DOC_VECTOR_DIM
        || entry.DDE_VectorDim as usize != DOC_VECTOR_DIM
        || entry.DDE_VectorFormat == 0
    {
        return 0.0;
    }

    let mut dot = 0.0f32;
    let mut nq = 0.0f32;
    let mut nd = 0.0f32;
    for i in 0..DOC_VECTOR_DIM {
        let q = query[i];
        let d = entry.DDE_VectorData[i] as f32 / 128.0;
        dot += q * d;
        nq += q * q;
        nd += d * d;
    }
    if nq <= 0.0 || nd <= 0.0 {
        return 0.0;
    }
    parameters.QMP_CosineWeight * dot / (nq.sqrt() * nd.sqrt())
}
