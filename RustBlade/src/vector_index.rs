use std::cmp::{Ordering, Reverse};
use std::collections::{BinaryHeap, HashMap, HashSet};

use serde::{Deserialize, Serialize};

use crate::block_table::{DOC_REC_SIZE, DOC_VECTOR_DIM};

const DOC_VECTOR_OFFSET: usize = 256;

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub enum Metric {
    L2,
    Cosine,
    DotProduct,
}

#[derive(Clone, Copy, PartialEq)]
struct F32(f32);

impl Eq for F32 {}

impl PartialOrd for F32 {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> { Some(self.cmp(other)) }
}

impl Ord for F32 {
    fn cmp(&self, other: &Self) -> Ordering {
        self.0.partial_cmp(&other.0).unwrap_or(Ordering::Equal)
    }
}

struct HnswNode {
    neighbors: Vec<Vec<usize>>,
}

pub struct HnswIndex {
    nodes: Vec<HnswNode>,
    entry_point: Option<usize>,
    max_layer: usize,
    max_neighbors: usize,
    ef_construction: usize,
    pub dim: usize,
    metric: Metric,
    docdata: Vec<u8>,
}

impl HnswIndex {
    pub fn new(dim: usize, max_neighbors: usize, ef_construction: usize, metric: Metric) -> Self {
        Self {
            nodes: Vec::new(),
            entry_point: None,
            max_layer: 0,
            max_neighbors: max_neighbors.max(2),
            ef_construction: ef_construction.max(max_neighbors.max(2)),
            dim,
            metric,
            docdata: Vec::new(),
        }
    }

    #[allow(non_snake_case)]
    pub fn SetDocData(&mut self, docdata: Vec<u8>) {
        self.docdata = docdata;
    }

    #[allow(non_snake_case)]
    pub fn Add(&mut self, doc_id: u64) -> bool {
        self.add_node(doc_id)
    }

    #[allow(non_snake_case)]
    pub fn Search(&self, query: &[f32], k: usize, ef: usize) -> Vec<(u64, f32)> {
        if query.len() != DOC_VECTOR_DIM || self.nodes.is_empty() { return Vec::new(); }

        let mut entry = self.entry_point.unwrap();
        for level in (1..=self.max_layer).rev() {
            entry = self.greedy_search_layer_query(query, entry, level);
        }

        let wanted = if k == 0 { self.nodes.len() } else { k };
        let mut candidates = self.search_layer_query(query, entry, ef.max(wanted), 0);

        if k == 0 && candidates.len() < self.nodes.len() {
            let mut seen = HashSet::new();
            for &(_, node_id) in &candidates { seen.insert(node_id); }
            for node_id in 0..self.nodes.len() {
                if !seen.contains(&node_id) {
                    candidates.push((self.score_query_to_node(query, node_id), node_id));
                }
            }
            candidates.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(Ordering::Equal).then(a.1.cmp(&b.1)));
        }

        let mut out: Vec<(u64, f32)> = candidates
            .into_iter()
            .map(|(_, node_id)| (node_id as u64, self.score_query_to_node(query, node_id)))
            .collect();
        out.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(Ordering::Equal).then(a.0.cmp(&b.0)));
        if k > 0 && out.len() > k { out.truncate(k); }
        out
    }

    #[allow(non_snake_case)]
    pub fn Size(&self) -> usize { self.nodes.len() }
    #[allow(non_snake_case)]
    pub fn Empty(&self) -> bool { self.nodes.is_empty() }

    fn add_node(&mut self, doc_id: u64) -> bool {
        if doc_id < self.nodes.len() as u64 { return true; }
        if doc_id != self.nodes.len() as u64 { return false; }

        let level = self.random_level(doc_id);
        let new_node_id = doc_id as usize;
        self.nodes.push(HnswNode {
            neighbors: (0..=level).map(|_| Vec::new()).collect(),
        });

        let Some(mut entry) = self.entry_point else {
            self.entry_point = Some(new_node_id);
            self.max_layer = level;
            return true;
        };

        for layer in (level + 1..=self.max_layer).rev() {
            entry = self.greedy_search_layer_doc(new_node_id, entry, layer);
        }

        for layer in (0..=level.min(self.max_layer)).rev() {
            let candidates = self.search_layer_doc(new_node_id, entry, self.ef_construction, layer);
            let selected: Vec<usize> = candidates.iter().take(self.max_neighbors).map(|&(_, id)| id).collect();

            self.nodes[new_node_id].neighbors[layer] = selected.clone();
            for neighbor in selected {
                self.link_back(neighbor, new_node_id, layer);
            }

            if let Some(&(_, nearest)) = candidates.first() {
                entry = nearest;
            }
        }

        if level > self.max_layer {
            self.entry_point = Some(new_node_id);
            self.max_layer = level;
        }
        true
    }

    fn get_doc_vector(&self, doc_id: usize) -> &[u8] {
        let offset = doc_id * DOC_REC_SIZE + DOC_VECTOR_OFFSET;
        &self.docdata[offset..offset + DOC_VECTOR_DIM]
    }

    fn score_query_to_node(&self, query: &[f32], node_id: usize) -> f32 {
        let doc = self.get_doc_vector(node_id);
        let mut dot = 0.0f32;
        let mut nq = 0.0f32;
        let mut nd = 0.0f32;
        let mut l2 = 0.0f32;
        for i in 0..DOC_VECTOR_DIM {
            let q = query[i];
            let d = (doc[i] as i8) as f32 / 128.0;
            dot += q * d;
            nq += q * q;
            nd += d * d;
            let delta = q - d;
            l2 += delta * delta;
        }
        match self.metric {
            Metric::DotProduct => dot,
            Metric::L2 => 1.0 / (1.0 + l2),
            Metric::Cosine => dot / (nq.sqrt() * nd.sqrt()),
        }
    }

    fn score_doc_to_node(&self, query_doc_id: usize, node_id: usize) -> f32 {
        let left = self.get_doc_vector(query_doc_id);
        let right = self.get_doc_vector(node_id);
        let mut dot = 0i32;
        let mut nl = 0i32;
        let mut nr = 0i32;
        let mut l2 = 0i32;
        for i in 0..DOC_VECTOR_DIM {
            let l = left[i] as i8 as i32;
            let r = right[i] as i8 as i32;
            dot += l * r;
            nl += l * l;
            nr += r * r;
            let delta = l - r;
            l2 += delta * delta;
        }
        match self.metric {
            Metric::DotProduct => dot as f32 / (128.0 * 128.0),
            Metric::L2 => 1.0 / (1.0 + l2 as f32 / (128.0 * 128.0)),
            Metric::Cosine => dot as f32 / ((nl as f32).sqrt() * (nr as f32).sqrt()),
        }
    }

    fn greedy_search_layer_query(&self, query: &[f32], entry: usize, layer: usize) -> usize {
        let mut best = entry;
        let mut best_score = self.score_query_to_node(query, best);
        loop {
            let mut changed = false;
            if layer >= self.nodes[best].neighbors.len() { break; }
            for &neighbor in &self.nodes[best].neighbors[layer] {
                let score = self.score_query_to_node(query, neighbor);
                if score > best_score {
                    best = neighbor;
                    best_score = score;
                    changed = true;
                }
            }
            if !changed { break; }
        }
        best
    }

    fn greedy_search_layer_doc(&self, query_doc_id: usize, entry: usize, layer: usize) -> usize {
        let mut best = entry;
        let mut best_score = self.score_doc_to_node(query_doc_id, best);
        loop {
            let mut changed = false;
            if layer >= self.nodes[best].neighbors.len() { break; }
            for &neighbor in &self.nodes[best].neighbors[layer] {
                let score = self.score_doc_to_node(query_doc_id, neighbor);
                if score > best_score {
                    best = neighbor;
                    best_score = score;
                    changed = true;
                }
            }
            if !changed { break; }
        }
        best
    }

    fn search_layer_query(&self, query: &[f32], entry: usize, ef: usize, layer: usize) -> Vec<(f32, usize)> {
        self.search_layer(entry, ef, layer, |node| self.score_query_to_node(query, node))
    }

    fn search_layer_doc(&self, query_doc_id: usize, entry: usize, ef: usize, layer: usize) -> Vec<(f32, usize)> {
        self.search_layer(entry, ef, layer, |node| self.score_doc_to_node(query_doc_id, node))
    }

    fn search_layer<F>(&self, entry: usize, ef: usize, layer: usize, score: F) -> Vec<(f32, usize)>
    where
        F: Fn(usize) -> f32,
    {
        let mut visited = HashSet::<usize>::new();
        let mut candidates: BinaryHeap<(F32, usize)> = BinaryHeap::new();
        let mut results: BinaryHeap<Reverse<(F32, usize)>> = BinaryHeap::new();

        let entry_score = score(entry);
        candidates.push((F32(entry_score), entry));
        results.push(Reverse((F32(entry_score), entry)));
        visited.insert(entry);

        while let Some((F32(current_score), current)) = candidates.pop() {
            if let Some(Reverse((F32(worst), _))) = results.peek() {
                if current_score < *worst { break; }
            }
            if layer >= self.nodes[current].neighbors.len() { continue; }
            for &neighbor in &self.nodes[current].neighbors[layer] {
                if !visited.insert(neighbor) { continue; }
                let neighbor_score = score(neighbor);
                if results.len() < ef || results.peek().map(|Reverse((F32(worst), _))| neighbor_score > *worst).unwrap_or(true) {
                    candidates.push((F32(neighbor_score), neighbor));
                    results.push(Reverse((F32(neighbor_score), neighbor)));
                    if results.len() > ef { results.pop(); }
                }
            }
        }

        let mut out: Vec<(f32, usize)> = results.into_iter().map(|Reverse((F32(score), id))| (score, id)).collect();
        out.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(Ordering::Equal).then(a.1.cmp(&b.1)));
        out
    }

    fn link_back(&mut self, node: usize, neighbor: usize, layer: usize) {
        while self.nodes[node].neighbors.len() <= layer {
            self.nodes[node].neighbors.push(Vec::new());
        }
        if !self.nodes[node].neighbors[layer].contains(&neighbor) {
            self.nodes[node].neighbors[layer].push(neighbor);
        }
        if self.nodes[node].neighbors[layer].len() > self.max_neighbors {
            let mut links = std::mem::take(&mut self.nodes[node].neighbors[layer]);
            links.sort_by(|&a, &b| {
                self.score_doc_to_node(node, b)
                    .partial_cmp(&self.score_doc_to_node(node, a))
                    .unwrap_or(Ordering::Equal)
                    .then(a.cmp(&b))
            });
            links.truncate(self.max_neighbors);
            self.nodes[node].neighbors[layer] = links;
        }
    }

    fn random_level(&self, doc_id: u64) -> usize {
        let mut hash = doc_id.wrapping_mul(11400714819323198485u64).wrapping_add(0x9e3779b97f4a7c15);
        let mut level = 0usize;
        while (hash & 0x3) == 0 && level < 16 {
            level += 1;
            hash >>= 2;
        }
        level
    }
}

pub enum VectorIndex {
    Hnsw(HnswIndex),
}

impl VectorIndex {
    pub fn hnsw(_dim: usize, metric: Metric) -> Self {
        VectorIndex::Hnsw(HnswIndex::new(DOC_VECTOR_DIM, 32, 200, metric))
    }

    pub fn hnsw_custom(_dim: usize, max_neighbors: usize, ef_construction: usize, metric: Metric) -> Self {
        VectorIndex::Hnsw(HnswIndex::new(DOC_VECTOR_DIM, max_neighbors, ef_construction, metric))
    }

    #[allow(non_snake_case)]
    pub fn SetDocData(&mut self, docdata: Vec<u8>) {
        match self {
            VectorIndex::Hnsw(h) => h.SetDocData(docdata),
        }
    }

    #[allow(non_snake_case)]
    pub fn Add(&mut self, doc_id: u64) -> bool {
        match self {
            VectorIndex::Hnsw(h) => h.Add(doc_id),
        }
    }

    #[allow(non_snake_case)]
    pub fn Search(&self, query: &[f32], k: usize, ef: usize) -> Vec<(u64, f32)> {
        match self {
            VectorIndex::Hnsw(h) => h.Search(query, k, ef),
        }
    }

    #[allow(non_snake_case)]
    pub fn Dimension(&self) -> usize { DOC_VECTOR_DIM }

    #[allow(non_snake_case)]
    pub fn Size(&self) -> usize {
        match self {
            VectorIndex::Hnsw(h) => h.Size(),
        }
    }

    #[allow(non_snake_case)]
    pub fn Empty(&self) -> bool { self.Size() == 0 }
}

pub fn build_hashed_embedding(tokens: &[String]) -> Vec<f32> {
    let mut result = vec![0.0f32; DOC_VECTOR_DIM];
    if tokens.is_empty() { return result; }

    let mut freq: HashMap<&str, usize> = HashMap::new();
    for token in tokens {
        if !token.is_empty() { *freq.entry(token.as_str()).or_insert(0) += 1; }
    }

    for (token, count) in freq {
        let slot = fnv_slot(token);
        let tf = 1.0 + (1.0 + count as f32).ln();
        let idf = 1.0 + (1.0 + 3.0 / token.len().max(1) as f32).ln();
        result[slot] += tf * idf;
    }

    let norm = result.iter().map(|v| v * v).sum::<f32>().sqrt();
    if norm > 0.0 {
        for value in &mut result { *value /= norm; }
    }
    result
}

fn fnv_slot(token: &str) -> usize {
    let mut hash = 14695981039346656037u64;
    for byte in token.bytes() {
        hash ^= byte as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash as usize % DOC_VECTOR_DIM
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::block_table::{DOC_REC_SIZE, DOC_VECTOR_DIM};

    fn docdata(vectors: &[[i8; DOC_VECTOR_DIM]]) -> Vec<u8> {
        let mut bytes = vec![0u8; vectors.len() * DOC_REC_SIZE];
        for (doc_id, vector) in vectors.iter().enumerate() {
            let offset = doc_id * DOC_REC_SIZE;
            bytes[offset..offset + 8].copy_from_slice(&(doc_id as u64).to_le_bytes());
            bytes[offset + 144..offset + 146].copy_from_slice(&(DOC_VECTOR_DIM as u16).to_le_bytes());
            bytes[offset + 146..offset + 148].copy_from_slice(&1u16.to_le_bytes());
            for i in 0..DOC_VECTOR_DIM {
                bytes[offset + DOC_VECTOR_OFFSET + i] = vector[i] as u8;
            }
        }
        bytes
    }

    #[test]
    fn hnsw_search_finds_nearest_from_docdata_bytes() {
        let mut vectors = Vec::new();
        for doc_id in 0..50 {
            let mut vector = [0i8; DOC_VECTOR_DIM];
            vector[0] = doc_id as i8;
            vectors.push(vector);
        }

        let mut idx = HnswIndex::new(DOC_VECTOR_DIM, 4, 10, Metric::L2);
        idx.SetDocData(docdata(&vectors));
        for doc_id in 0u64..50 {
            assert!(idx.Add(doc_id));
        }

        let mut query = vec![0.0f32; DOC_VECTOR_DIM];
        query[0] = 25.0 / 128.0;
        let results = idx.Search(&query, 1, 20);
        assert!(!results.is_empty());
        assert_eq!(results[0].0, 25);
    }
}
