use std::collections::{BinaryHeap, HashSet};
use std::cmp::{Ordering, Reverse};
use rand::Rng;
use serde::{Deserialize, Serialize};

// ---------------------------------------------------------------------------
// Distance metrics
// ---------------------------------------------------------------------------
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub enum Metric {
    L2,
    Cosine,
    DotProduct,
}

pub fn l2_sq(a: &[f32], b: &[f32]) -> f32 {
    a.iter().zip(b).map(|(x, y)| (x - y).powi(2)).sum()
}

pub fn dot(a: &[f32], b: &[f32]) -> f32 {
    a.iter().zip(b).map(|(x, y)| x * y).sum()
}

pub fn cosine(a: &[f32], b: &[f32]) -> f32 {
    let d = dot(a, b);
    let na = a.iter().map(|x| x.powi(2)).sum::<f32>().sqrt();
    let nb = b.iter().map(|x| x.powi(2)).sum::<f32>().sqrt();
    if na == 0.0 || nb == 0.0 { 0.0 } else { d / (na * nb) }
}

// ---------------------------------------------------------------------------
// Ordered f32 wrapper for BinaryHeap (f32 is not Ord).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// FlatVectorIndex — exact brute-force O(N·d) search.
// Good up to ~10k vectors; guaranteed correct.
// ---------------------------------------------------------------------------
#[derive(Serialize, Deserialize)]
pub struct FlatVectorIndex {
    entries: Vec<(u64, Vec<f32>)>,
    pub dim:    usize,
    metric:  Metric,
}

impl FlatVectorIndex {
    pub fn new(dim: usize, metric: Metric) -> Self {
        Self { entries: Vec::new(), dim, metric }
    }

    pub fn add(&mut self, doc_id: u64, vector: Vec<f32>) {
        self.entries.push((doc_id, vector));
    }

    pub fn len(&self) -> usize { self.entries.len() }
    pub fn is_empty(&self) -> bool { self.entries.is_empty() }

    /// Return up to `k` (doc_id, score) pairs ranked highest first.
    pub fn search(&self, query: &[f32], k: usize) -> Vec<(u64, f32)> {
        let mut heap: BinaryHeap<Reverse<(F32, u64)>> = BinaryHeap::new();
        for (id, v) in &self.entries {
            let score = self.score(query, v);
            heap.push(Reverse((F32(score), *id)));
            if heap.len() > k {
                heap.pop();
            }
        }
        let mut out: Vec<(u64, f32)> = heap.into_sorted_vec()
            .into_iter()
            .map(|Reverse((F32(s), id))| (id, s))
            .collect();
        out.reverse(); // sorted_vec is ascending; we want descending
        out
    }

    fn score(&self, a: &[f32], b: &[f32]) -> f32 {
        match self.metric {
            Metric::L2          => 1.0 / (1.0 + l2_sq(a, b)),
            Metric::Cosine      => cosine(a, b),
            Metric::DotProduct  => dot(a, b),
        }
    }
}

// ---------------------------------------------------------------------------
// HnswIndex — Hierarchical Navigable Small World graph.
//
// Implements the algorithm from Malkov & Yashunin (2020).
// Default M=16, ef_construction=200 match production settings in InfinityDB.
// ---------------------------------------------------------------------------
#[derive(Clone, Serialize, Deserialize)]
struct HnswNode {
    doc_id:    u64,
    vector:    Vec<f32>,
    /// neighbors[layer] = list of node indices in self.nodes
    neighbors: Vec<Vec<usize>>,
}

#[derive(Serialize, Deserialize)]
pub struct HnswIndex {
    nodes:          Vec<HnswNode>,
    entry_point:    Option<usize>,
    max_layer:      usize,
    m:              usize,   // max neighbours per layer (except layer 0)
    m0:             usize,   // max neighbours at layer 0  (= 2*m)
    ef_construction: usize,
    ml:             f64,     // 1 / ln(m) — level generation multiplier
    pub dim:        usize,
    metric:         Metric,
}

impl HnswIndex {
    pub fn new(dim: usize, m: usize, ef_construction: usize, metric: Metric) -> Self {
        let m = m.max(2);
        Self {
            nodes: Vec::new(),
            entry_point: None,
            max_layer: 0,
            m,
            m0: m * 2,
            ef_construction,
            ml: 1.0 / (m as f64).ln(),
            dim,
            metric,
        }
    }

    pub fn len(&self) -> usize { self.nodes.len() }
    pub fn is_empty(&self) -> bool { self.nodes.is_empty() }

    // -- insertion ----------------------------------------------------------

    pub fn add(&mut self, doc_id: u64, vector: Vec<f32>) {
        let new_idx  = self.nodes.len();
        let level    = self.random_level();

        // Initialise node with empty neighbour lists up to `level`.
        self.nodes.push(HnswNode {
            doc_id,
            vector,
            neighbors: (0..=level).map(|_| Vec::new()).collect(),
        });

        let Some(ep) = self.entry_point else {
            self.entry_point = Some(new_idx);
            self.max_layer   = level;
            return;
        };

        let mut ep_idx = ep;
        let top        = self.max_layer;

        // Phase 1: greedy descent from top_layer down to level+1 (ef=1).
        for lc in (level + 1..=top).rev() {
            let cands = self.search_layer_(&self.nodes[new_idx].vector, ep_idx, 1, lc);
            if let Some(&(_, nearest)) = cands.first() {
                ep_idx = nearest;
            }
        }

        // Phase 2: build connections at each layer from min(level,top) to 0.
        for lc in (0..=level.min(top)).rev() {
            let m_max = if lc == 0 { self.m0 } else { self.m };
            let query: Vec<f32> = self.nodes[new_idx].vector.clone();
            let cands = self.search_layer_(&query, ep_idx, self.ef_construction, lc);

            let selected: Vec<usize> = cands.iter().take(m_max).map(|&(_, i)| i).collect();

            // Store this node's neighbours.
            if lc < self.nodes[new_idx].neighbors.len() {
                self.nodes[new_idx].neighbors[lc] = selected.clone();
            }

            // Back-links: add new_idx to each selected neighbour's list and prune.
            let mut back_updates: Vec<(usize, Vec<usize>)> = Vec::new();
            for &nb in &selected {
                let mut conns = if lc < self.nodes[nb].neighbors.len() {
                    self.nodes[nb].neighbors[lc].clone()
                } else {
                    Vec::new()
                };
                conns.push(new_idx);

                if conns.len() > m_max {
                    // Prune: keep the m_max closest to `nb`.
                    let nb_vec: Vec<f32> = self.nodes[nb].vector.clone();
                    conns.sort_by(|&a, &b| {
                        let da = self.dist(&self.nodes[a].vector, &nb_vec);
                        let db = self.dist(&self.nodes[b].vector, &nb_vec);
                        da.partial_cmp(&db).unwrap_or(Ordering::Equal)
                    });
                    conns.truncate(m_max);
                }
                back_updates.push((nb, conns));
            }

            for (nb, conns) in back_updates {
                while self.nodes[nb].neighbors.len() <= lc {
                    self.nodes[nb].neighbors.push(Vec::new());
                }
                self.nodes[nb].neighbors[lc] = conns;
            }

            // Carry the best candidate as entry point for the next layer.
            if let Some(&(_, nearest)) = cands.first() {
                ep_idx = nearest;
            }
        }

        // Update global entry point if the new node has a higher level.
        if level > self.max_layer {
            self.max_layer   = level;
            self.entry_point = Some(new_idx);
        }
    }

    // -- search -------------------------------------------------------------

    /// Approximate nearest-neighbour search.
    /// `ef` controls recall vs speed (try 50–200).
    pub fn search(&self, query: &[f32], k: usize, ef: usize) -> Vec<(u64, f32)> {
        let Some(ep) = self.entry_point else { return vec![]; };

        let mut ep_idx = ep;

        // Greedy descent from max_layer down to layer 1 (ef=1).
        for lc in (1..=self.max_layer).rev() {
            let cands = self.search_layer_(query, ep_idx, 1, lc);
            if let Some(&(_, nearest)) = cands.first() {
                ep_idx = nearest;
            }
        }

        // Full beam search at layer 0.
        let ef_search = ef.max(k);
        let cands = self.search_layer_(query, ep_idx, ef_search, 0);

        cands.into_iter().take(k).map(|(dist, idx)| {
            let score = self.dist_to_score(dist);
            (self.nodes[idx].doc_id, score)
        }).collect()
    }

    // -- private helpers ----------------------------------------------------

    fn dist(&self, a: &[f32], b: &[f32]) -> f32 {
        match self.metric {
            Metric::L2         => l2_sq(a, b),
            Metric::Cosine     => 1.0 - cosine(a, b),  // distance = 1 − similarity
            Metric::DotProduct => -dot(a, b),
        }
    }

    fn dist_to_score(&self, dist: f32) -> f32 {
        match self.metric {
            Metric::L2         => 1.0 / (1.0 + dist),
            Metric::Cosine     => 1.0 - dist,
            Metric::DotProduct => -dist,
        }
    }

    fn random_level(&self) -> usize {
        let mut rng = rand::thread_rng();
        let mut level = 0usize;
        while rng.gen::<f64>() < self.ml && level < 16 {
            level += 1;
        }
        level
    }

    /// Beam search within one layer; returns (distance, node_idx) sorted
    /// ascending by distance (closest first).
    fn search_layer_(&self, query: &[f32], entry: usize, ef: usize, layer: usize) -> Vec<(f32, usize)> {
        let mut visited  = HashSet::<usize>::new();
        visited.insert(entry);

        let entry_dist = self.dist(query, &self.nodes[entry].vector);

        // candidates: min-heap (Reverse wrapper).
        let mut candidates: BinaryHeap<Reverse<(F32, usize)>> = BinaryHeap::new();
        // results: max-heap — evict the worst (farthest) when over capacity.
        let mut results: BinaryHeap<(F32, usize)> = BinaryHeap::new();

        candidates.push(Reverse((F32(entry_dist), entry)));
        results.push((F32(entry_dist), entry));

        while let Some(Reverse((F32(c_dist), c_idx))) = candidates.pop() {
            // Pruning: the candidate is farther than the worst in results.
            if results.len() >= ef {
                if let Some(&(F32(worst), _)) = results.peek() {
                    if c_dist > worst {
                        break;
                    }
                }
            }

            let layer_ok = layer < self.nodes[c_idx].neighbors.len();
            if !layer_ok { continue; }

            for &nb in &self.nodes[c_idx].neighbors[layer] {
                if visited.contains(&nb) { continue; }
                visited.insert(nb);

                let nb_dist = self.dist(query, &self.nodes[nb].vector);

                let should_add = results.len() < ef || {
                    results.peek().map(|&(F32(w), _)| nb_dist < w).unwrap_or(true)
                };

                if should_add {
                    candidates.push(Reverse((F32(nb_dist), nb)));
                    results.push((F32(nb_dist), nb));
                    if results.len() > ef {
                        results.pop();
                    }
                }
            }
        }

        let mut out: Vec<(f32, usize)> = results.into_iter().map(|(F32(d), i)| (d, i)).collect();
        out.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(Ordering::Equal));
        out
    }
}

// ---------------------------------------------------------------------------
// VectorIndex — public enum unifying Flat and HNSW.
// ---------------------------------------------------------------------------
#[derive(Serialize, Deserialize)]
pub enum VectorIndex {
    Flat(FlatVectorIndex),
    Hnsw(HnswIndex),
}

impl VectorIndex {
    pub fn flat(dim: usize, metric: Metric) -> Self {
        VectorIndex::Flat(FlatVectorIndex::new(dim, metric))
    }

    pub fn hnsw(dim: usize, metric: Metric) -> Self {
        VectorIndex::Hnsw(HnswIndex::new(dim, 16, 200, metric))
    }

    pub fn hnsw_custom(dim: usize, m: usize, ef_construction: usize, metric: Metric) -> Self {
        VectorIndex::Hnsw(HnswIndex::new(dim, m, ef_construction, metric))
    }

    pub fn add(&mut self, doc_id: u64, vector: Vec<f32>) {
        match self {
            VectorIndex::Flat(f) => f.add(doc_id, vector),
            VectorIndex::Hnsw(h) => h.add(doc_id, vector),
        }
    }

    /// Search for the `k` approximate nearest neighbours.
    /// `ef` is the beam width for HNSW (ignored for Flat).
    pub fn search(&self, query: &[f32], k: usize, ef: usize) -> Vec<(u64, f32)> {
        match self {
            VectorIndex::Flat(f) => f.search(query, k),
            VectorIndex::Hnsw(h) => h.search(query, k, ef),
        }
    }

    pub fn dim(&self) -> usize {
        match self {
            VectorIndex::Flat(f) => f.dim,
            VectorIndex::Hnsw(h) => h.dim,
        }
    }

    pub fn len(&self) -> usize {
        match self {
            VectorIndex::Flat(f) => f.len(),
            VectorIndex::Hnsw(h) => h.len(),
        }
    }

    pub fn is_empty(&self) -> bool { self.len() == 0 }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flat_search_returns_correct_nearest() {
        let mut idx = FlatVectorIndex::new(2, Metric::L2);
        idx.add(1, vec![0.0, 0.0]);
        idx.add(2, vec![1.0, 0.0]);
        idx.add(3, vec![0.0, 1.0]);
        let results = idx.search(&[0.1, 0.1], 1);
        assert_eq!(results[0].0, 1); // nearest to origin
    }

    #[test]
    fn hnsw_search_finds_nearest() {
        let mut idx = HnswIndex::new(2, 4, 10, Metric::L2);
        for i in 0u64..50 {
            idx.add(i, vec![i as f32, 0.0]);
        }
        let results = idx.search(&[25.1, 0.0], 1, 20);
        assert!(!results.is_empty());
        assert_eq!(results[0].0, 25);
    }
}
