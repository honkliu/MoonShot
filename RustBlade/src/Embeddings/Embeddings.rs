//! Direct translation of the C++ embedding layer; symbol names stay aligned for debugging.
#![allow(non_snake_case, non_upper_case_globals)]

use std::cmp::{Ordering, Reverse};
use std::collections::{BinaryHeap, HashSet};
use std::sync::Arc;

use serde::{Deserialize, Serialize};

use crate::block_table::{DOC_REC_SIZE, DOC_VECTOR_DIM, DOC_VECTOR_OFFSET};
use crate::posting_store::StableHashMap;

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub enum VectorMetric {
    #[default]
    Cosine,
    DotProduct,
    L2,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct VectorSearchResult {
    pub doc_id: u64,
    pub score: f32,
}

pub trait IEmbeddingModel: Send + Sync {
    #[allow(non_snake_case)]
    fn Embed(&self, tokens: &[String]) -> Vec<f32>;
    #[allow(non_snake_case)]
    fn GetDimension(&self) -> usize {
        DOC_VECTOR_DIM
    }
}

pub struct TFIDFSemanticEmbedding {
    m_Dim: usize,
}
impl TFIDFSemanticEmbedding {
    pub fn new(dim: usize) -> Self {
        Self { m_Dim: dim }
    }
    fn GetSlotForToken(&self, token: &str) -> usize {
        let mut hash = 14695981039346656037u64;
        for byte in token.bytes() {
            hash ^= byte as u64;
            hash = hash.wrapping_mul(1099511628211);
        }
        hash as usize % self.m_Dim
    }
}
#[allow(non_snake_case)]
impl IEmbeddingModel for TFIDFSemanticEmbedding {
    fn Embed(&self, tokens: &[String]) -> Vec<f32> {
        let mut result = vec![0.0; self.m_Dim];
        if tokens.is_empty() {
            return result;
        }
        let mut tokenFreq: StableHashMap<&str, usize> = StableHashMap::default();
        for token in tokens {
            if !token.is_empty() {
                *tokenFreq.entry(token).or_insert(0) += 1;
            }
        }
        for (token, freq) in tokenFreq {
            let tfWeight = 1.0 + (1.0 + freq as f32).ln();
            let idfAdjust = 1.0 + (1.0 + 3.0 / token.len() as f32).ln();
            result[self.GetSlotForToken(token)] += tfWeight * idfAdjust;
        }
        let norm = result.iter().map(|value| value * value).sum::<f32>().sqrt();
        if norm > 0.0 {
            for value in &mut result {
                *value /= norm;
            }
        }
        result
    }
    fn GetDimension(&self) -> usize {
        self.m_Dim
    }
}

#[derive(Clone, Copy, PartialEq)]
struct F32(f32);
impl Eq for F32 {}
impl PartialOrd for F32 {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for F32 {
    fn cmp(&self, other: &Self) -> Ordering {
        self.0.partial_cmp(&other.0).unwrap_or(Ordering::Equal)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct ScoredNode {
    score: F32,
    node_id: u32,
}
impl PartialOrd for ScoredNode {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for ScoredNode {
    fn cmp(&self, other: &Self) -> Ordering {
        self.score.cmp(&other.score)
    }
}

fn CompareScoreThenDocId(
    left_score: f32,
    left_id: u32,
    right_score: f32,
    right_id: u32,
) -> Ordering {
    if left_score != right_score {
        if left_score > right_score {
            Ordering::Less
        } else {
            Ordering::Greater
        }
    } else {
        left_id.cmp(&right_id)
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct Node {
    pub level: u8,
    pub linkOffset: u32,
}

#[derive(Clone)]
pub struct FreshDiskAnnVectorIndex {
    m_Dim: usize,
    m_MaxNeighbors: usize,
    m_EfConstruction: usize,
    m_Nodes: Vec<Node>,
    m_DocIds: Vec<u32>,
    m_Links: Vec<u32>,
    m_LinkCounts: Vec<u8>,
    m_DocData: Arc<[u8]>,
    m_DocDataFirstDocId: u64,
    m_EntryPoint: u32,
    m_MaxLevel: usize,
    m_Model: Arc<dyn IEmbeddingModel>,
}
impl Default for FreshDiskAnnVectorIndex {
    fn default() -> Self {
        Self::new(32, 200)
    }
}

#[allow(non_snake_case)]
impl FreshDiskAnnVectorIndex {
    pub fn new(maxNeighbors: usize, efConstruction: usize) -> Self {
        let maxNeighbors = maxNeighbors.clamp(2, u8::MAX as usize);
        Self {
            m_Dim: DOC_VECTOR_DIM,
            m_MaxNeighbors: maxNeighbors,
            m_EfConstruction: efConstruction.max(maxNeighbors),
            m_Nodes: Vec::new(),
            m_DocIds: Vec::new(),
            m_Links: Vec::new(),
            m_LinkCounts: Vec::new(),
            m_DocData: Arc::from([]),
            m_DocDataFirstDocId: 0,
            m_EntryPoint: u32::MAX,
            m_MaxLevel: 0,
            m_Model: Arc::new(TFIDFSemanticEmbedding::new(DOC_VECTOR_DIM)),
        }
    }
    pub fn WithModel(
        maxNeighbors: usize,
        efConstruction: usize,
        model: Arc<dyn IEmbeddingModel>,
    ) -> Self {
        let mut index = Self::new(maxNeighbors, efConstruction);
        index.m_Model = model;
        index
    }
    pub fn Clear(&mut self) {
        self.m_Dim = DOC_VECTOR_DIM;
        self.m_Nodes.clear();
        self.m_DocIds.clear();
        self.m_Links.clear();
        self.m_LinkCounts.clear();
        self.m_DocData = Arc::from([]);
        self.m_EntryPoint = u32::MAX;
        self.m_MaxLevel = 0;
    }
    pub fn SetDocData(&mut self, docData: Arc<[u8]>, firstDocId: u64) {
        self.m_DocData = docData;
        self.m_DocDataFirstDocId = firstDocId;
    }
    pub fn Add(&mut self, docId: u64) -> bool {
        self.AddNode(docId)
    }
    pub fn Reserve(&mut self, nodeCount: usize) {
        self.m_Nodes.reserve(nodeCount);
        self.m_DocIds.reserve(nodeCount);
        let expectedLayers = nodeCount.saturating_add(nodeCount.saturating_add(2) / 3);
        self.m_LinkCounts.reserve(expectedLayers);
        if expectedLayers <= u32::MAX as usize / self.m_MaxNeighbors {
            self.m_Links.reserve(expectedLayers * self.m_MaxNeighbors);
        }
    }
    pub fn Search(
        &self,
        query: &[f32],
        topK: usize,
        metric: VectorMetric,
        efSearch: usize,
    ) -> Vec<VectorSearchResult> {
        if query.len() != DOC_VECTOR_DIM || self.m_Nodes.is_empty() {
            return Vec::new();
        }
        let mut entry = self.m_EntryPoint;
        for level in (1..=self.m_MaxLevel).rev() {
            entry = self.GreedySearchLayerQuery(query, entry, level, metric);
        }
        let wanted = if topK == 0 { self.m_Nodes.len() } else { topK };
        let mut candidates = self.SearchLayerQuery(query, entry, efSearch.max(wanted), 0, metric);
        if topK == 0 && candidates.len() < self.m_Nodes.len() {
            let seen: HashSet<u32> = candidates.iter().map(|candidate| candidate.1).collect();
            for nodeID in 0..self.m_Nodes.len() as u32 {
                if !seen.contains(&nodeID) {
                    candidates.push((self.ScoreQuery(nodeID, query, metric), nodeID));
                }
            }
        }
        let mut results: Vec<_> = candidates
            .into_iter()
            .map(|candidate| VectorSearchResult {
                doc_id: self.m_DocIds[candidate.1 as usize] as u64,
                score: self.ScoreQuery(candidate.1, query, metric),
            })
            .collect();
        results.sort_by(|a, b| {
            CompareScoreThenDocId(a.score, a.doc_id as u32, b.score, b.doc_id as u32)
        });
        if topK > 0 && results.len() > topK {
            results.truncate(topK);
        }
        results
    }
    pub fn Dimension(&self) -> usize {
        self.m_Dim
    }
    pub fn Size(&self) -> usize {
        self.m_Nodes.len()
    }
    pub fn Empty(&self) -> bool {
        self.m_Nodes.is_empty()
    }
    pub fn GetModel(&self) -> &dyn IEmbeddingModel {
        self.m_Model.as_ref()
    }
    pub fn MaxLevel(&self) -> usize {
        self.m_MaxLevel
    }
    pub fn MaxNeighbors(&self) -> usize {
        self.m_MaxNeighbors
    }
    pub fn EfConstruction(&self) -> usize {
        self.m_EfConstruction
    }
    pub fn Nodes(&self) -> &[Node] {
        &self.m_Nodes
    }
    pub fn ScoreQueryToDoc(query: &[f32], doc: &[u8], metric: VectorMetric) -> f32 {
        let mut dot = 0.0;
        let mut nq = 0.0;
        let mut nd = 0.0;
        let mut l2 = 0.0;
        for (&q, &encoded) in query.iter().zip(doc).take(DOC_VECTOR_DIM) {
            let d = encoded as i8 as f32 / 128.0;
            dot += q * d;
            nq += q * q;
            nd += d * d;
            let delta = q - d;
            l2 += delta * delta;
        }
        match metric {
            VectorMetric::DotProduct => dot,
            VectorMetric::L2 => 1.0 / (1.0 + l2),
            VectorMetric::Cosine => dot / (nq.sqrt() * nd.sqrt()),
        }
    }
    pub fn ScoreDocToDoc(left: &[u8], right: &[u8], metric: VectorMetric) -> f32 {
        let mut dot = 0i32;
        let mut nl = 0i32;
        let mut nr = 0i32;
        let mut l2 = 0i32;
        for (&l, &r) in left.iter().zip(right).take(DOC_VECTOR_DIM) {
            let l = l as i8 as i32;
            let r = r as i8 as i32;
            dot += l * r;
            nl += l * l;
            nr += r * r;
            let delta = l - r;
            l2 += delta * delta;
        }
        match metric {
            VectorMetric::DotProduct => dot as f32 / (128.0 * 128.0),
            VectorMetric::L2 => 1.0 / (1.0 + l2 as f32 / (128.0 * 128.0)),
            VectorMetric::Cosine => dot as f32 / ((nl as f32).sqrt() * (nr as f32).sqrt()),
        }
    }
    fn AddNode(&mut self, docId: u64) -> bool {
        if docId > u32::MAX as u64 || self.m_Nodes.len() >= u32::MAX as usize {
            return false;
        }
        let mut node = Node {
            level: self.RandomLevel(docId) as u8,
            linkOffset: 0,
        };
        if !self.AllocateLinks(&mut node) {
            return false;
        }
        let newNodeID = self.m_Nodes.len() as u32;
        self.m_DocIds.push(docId as u32);
        self.m_Nodes.push(node);
        if self.m_EntryPoint == u32::MAX {
            self.m_EntryPoint = newNodeID;
            self.m_MaxLevel = self.NodeLevel(newNodeID);
            return true;
        }
        let mut entry = self.m_EntryPoint;
        if self.NodeLevel(newNodeID) < self.m_MaxLevel {
            for level in ((self.NodeLevel(newNodeID) + 1)..=self.m_MaxLevel).rev() {
                entry = self.GreedySearchLayerDoc(newNodeID, entry, level);
            }
        }
        for level in (0..=self.m_MaxLevel.min(self.NodeLevel(newNodeID))).rev() {
            let candidates = self.SearchLayerDoc(newNodeID, entry, self.m_EfConstruction, level);
            let selected = self.SelectNeighbors(&candidates, self.m_MaxNeighbors);
            self.SetNeighbors(newNodeID, level, &selected);
            for neighbor in selected {
                self.LinkBack(neighbor, newNodeID, level);
            }
            if let Some(candidate) = candidates.first() {
                entry = candidate.1;
            }
        }
        if self.NodeLevel(newNodeID) > self.m_MaxLevel {
            self.m_EntryPoint = newNodeID;
            self.m_MaxLevel = self.NodeLevel(newNodeID);
        }
        true
    }
    fn AllocateLinks(&mut self, node: &mut Node) -> bool {
        let layerCount = node.level as usize + 1;
        let Some(slotCount) = layerCount.checked_mul(self.m_MaxNeighbors) else {
            return false;
        };
        if slotCount > u32::MAX as usize || self.m_Links.len() > u32::MAX as usize - slotCount {
            return false;
        }
        node.linkOffset = self.m_Links.len() as u32;
        self.m_Links
            .resize(self.m_Links.len() + slotCount, u32::MAX);
        self.m_LinkCounts
            .resize(self.m_LinkCounts.len() + layerCount, 0);
        true
    }
    fn GetDocVector(&self, docId: u64) -> &[u8] {
        let offset = (docId - self.m_DocDataFirstDocId) as usize * DOC_REC_SIZE + DOC_VECTOR_OFFSET;
        &self.m_DocData[offset..offset + DOC_VECTOR_DIM]
    }
    fn GetNodeVector(&self, nodeID: u32) -> &[u8] {
        self.GetDocVector(self.m_DocIds[nodeID as usize] as u64)
    }
    fn NodeLevel(&self, nodeID: u32) -> usize {
        self.m_Nodes[nodeID as usize].level as usize
    }
    fn HasLevel(&self, nodeID: u32, level: usize) -> bool {
        level <= self.NodeLevel(nodeID)
    }
    fn LinkOffset(&self, nodeID: u32, level: usize) -> usize {
        self.m_Nodes[nodeID as usize].linkOffset as usize + level * self.m_MaxNeighbors
    }
    fn LinkCountIndex(&self, nodeID: u32, level: usize) -> usize {
        self.m_Nodes[nodeID as usize].linkOffset as usize / self.m_MaxNeighbors + level
    }
    fn Neighbors(&self, nodeID: u32, level: usize) -> &[u32] {
        if !self.HasLevel(nodeID, level) {
            return &[];
        }
        let offset = self.LinkOffset(nodeID, level);
        let count = self.m_LinkCounts[self.LinkCountIndex(nodeID, level)] as usize;
        &self.m_Links[offset..offset + count]
    }
    fn SetNeighbors(&mut self, nodeID: u32, level: usize, neighbors: &[u32]) {
        if !self.HasLevel(nodeID, level) {
            return;
        }
        let offset = self.LinkOffset(nodeID, level);
        let count = neighbors.len().min(self.m_MaxNeighbors);
        self.m_Links[offset..offset + self.m_MaxNeighbors].fill(u32::MAX);
        self.m_Links[offset..offset + count].copy_from_slice(&neighbors[..count]);
        let countIndex = self.LinkCountIndex(nodeID, level);
        self.m_LinkCounts[countIndex] = count as u8;
    }
    fn ScoreQuery(&self, nodeID: u32, query: &[f32], metric: VectorMetric) -> f32 {
        Self::ScoreQueryToDoc(query, self.GetNodeVector(nodeID), metric)
    }
    fn ScoreDoc(&self, queryNodeID: u32, nodeID: u32, metric: VectorMetric) -> f32 {
        let left = self.GetNodeVector(queryNodeID);
        let right = self.GetNodeVector(nodeID);
        Self::ScoreDocToDoc(left, right, metric)
    }
    fn GreedySearchLayerQuery(
        &self,
        query: &[f32],
        entry: u32,
        level: usize,
        metric: VectorMetric,
    ) -> u32 {
        self.GreedySearchLayer(entry, level, |nodeID| {
            self.ScoreQuery(nodeID, query, metric)
        })
    }
    fn GreedySearchLayerDoc(&self, queryNodeID: u32, entry: u32, level: usize) -> u32 {
        self.GreedySearchLayer(entry, level, |nodeID| {
            self.ScoreDoc(queryNodeID, nodeID, VectorMetric::Cosine)
        })
    }
    fn GreedySearchLayer<F>(&self, entry: u32, level: usize, score: F) -> u32
    where
        F: Fn(u32) -> f32,
    {
        let mut best = entry;
        let mut bestScore = score(best);
        loop {
            let mut changed = false;
            for &neighbor in self.Neighbors(best, level) {
                let s = score(neighbor);
                if s > bestScore {
                    best = neighbor;
                    bestScore = s;
                    changed = true;
                }
            }
            if !changed {
                return best;
            }
        }
    }
    fn SearchLayerQuery(
        &self,
        query: &[f32],
        entry: u32,
        ef: usize,
        level: usize,
        metric: VectorMetric,
    ) -> Vec<(f32, u32)> {
        self.SearchLayer(entry, ef, level, |nodeID| {
            self.ScoreQuery(nodeID, query, metric)
        })
    }
    fn SearchLayerDoc(
        &self,
        queryNodeID: u32,
        entry: u32,
        ef: usize,
        level: usize,
    ) -> Vec<(f32, u32)> {
        self.SearchLayer(entry, ef, level, |nodeID| {
            self.ScoreDoc(queryNodeID, nodeID, VectorMetric::Cosine)
        })
    }
    fn SearchLayer<F>(&self, entry: u32, ef: usize, level: usize, score: F) -> Vec<(f32, u32)>
    where
        F: Fn(u32) -> f32,
    {
        let mut candidates = BinaryHeap::new();
        let mut results = BinaryHeap::new();
        let mut visited = HashSet::new();
        let entryScore = score(entry);
        candidates.push(ScoredNode {
            score: F32(entryScore),
            node_id: entry,
        });
        results.push(Reverse(ScoredNode {
            score: F32(entryScore),
            node_id: entry,
        }));
        visited.insert(entry);
        while let Some(ScoredNode {
            score: F32(currentScore),
            node_id: current,
        }) = candidates.pop()
        {
            if results
                .peek()
                .map(|Reverse(entry)| currentScore < entry.score.0)
                .unwrap_or(false)
            {
                break;
            }
            for &neighbor in self.Neighbors(current, level) {
                if !visited.insert(neighbor) {
                    continue;
                }
                let s = score(neighbor);
                if results.len() < ef
                    || results
                        .peek()
                        .map(|Reverse(entry)| s > entry.score.0)
                        .unwrap_or(true)
                {
                    candidates.push(ScoredNode {
                        score: F32(s),
                        node_id: neighbor,
                    });
                    results.push(Reverse(ScoredNode {
                        score: F32(s),
                        node_id: neighbor,
                    }));
                    if results.len() > ef {
                        results.pop();
                    }
                }
            }
        }
        let mut out: Vec<_> = results
            .into_iter()
            .map(|Reverse(entry)| (entry.score.0, entry.node_id))
            .collect();
        out.sort_by(|a, b| CompareScoreThenDocId(a.0, a.1, b.0, b.1));
        out
    }
    fn SelectNeighbors(&self, candidates: &[(f32, u32)], maxNeighbors: usize) -> Vec<u32> {
        let mut sorted = candidates.to_vec();
        sorted.sort_by(|a, b| CompareScoreThenDocId(a.0, a.1, b.0, b.1));
        sorted
            .into_iter()
            .take(maxNeighbors)
            .map(|candidate| candidate.1)
            .collect()
    }
    fn SortLayerByScore(&mut self, nodeID: u32, level: usize) {
        let offset = self.LinkOffset(nodeID, level);
        let count = self.m_LinkCounts[self.LinkCountIndex(nodeID, level)] as usize;
        let mut links = self.m_Links[offset..offset + count].to_vec();
        links.sort_by(|&a, &b| {
            CompareScoreThenDocId(
                self.ScoreDoc(nodeID, a, VectorMetric::Cosine),
                a,
                self.ScoreDoc(nodeID, b, VectorMetric::Cosine),
                b,
            )
        });
        self.m_Links[offset..offset + count].copy_from_slice(&links);
    }
    fn LinkBack(&mut self, nodeID: u32, neighbor: u32, level: usize) {
        if !self.HasLevel(nodeID, level) {
            return;
        }
        let offset = self.LinkOffset(nodeID, level);
        let countIndex = self.LinkCountIndex(nodeID, level);
        let count = self.m_LinkCounts[countIndex] as usize;
        if self.m_Links[offset..offset + count].contains(&neighbor) {
            return;
        }
        if count < self.m_MaxNeighbors {
            self.m_Links[offset + count] = neighbor;
            self.m_LinkCounts[countIndex] = (count + 1) as u8;
            return;
        }
        let neighborScore = self.ScoreDoc(nodeID, neighbor, VectorMetric::Cosine);
        let mut worstIndex = 0;
        let mut worstScore = self.ScoreDoc(nodeID, self.m_Links[offset], VectorMetric::Cosine);
        for index in 1..count {
            let candidate = self.m_Links[offset + index];
            let s = self.ScoreDoc(nodeID, candidate, VectorMetric::Cosine);
            if s < worstScore || (s == worstScore && candidate > self.m_Links[offset + worstIndex])
            {
                worstIndex = index;
                worstScore = s;
            }
        }
        if neighborScore > worstScore
            || (neighborScore == worstScore && neighbor < self.m_Links[offset + worstIndex])
        {
            self.m_Links[offset + worstIndex] = neighbor;
            self.SortLayerByScore(nodeID, level);
        }
    }
    fn RandomLevel(&self, docId: u64) -> usize {
        let mut hash = docId
            .wrapping_mul(11400714819323198485)
            .wrapping_add(0x9e3779b97f4a7c15);
        let mut level = 0;
        while hash & 3 == 0 && level < 16 {
            level += 1;
            hash >>= 2;
        }
        level
    }
}

pub fn build_hashed_embedding(tokens: &[String]) -> Vec<f32> {
    TFIDFSemanticEmbedding::new(DOC_VECTOR_DIM).Embed(tokens)
}
