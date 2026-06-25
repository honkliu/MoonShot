use std::collections::HashMap;

// Rust-only ranking helper. C++ MoonShot currently does text/vector combination
// through reader composition, so this module is intentionally outside the C++ file map.

/// Reciprocal Rank Fusion — merges multiple ranked lists into one.
///
/// RRF_score(doc) = Σ_r  1 / (k + rank(doc, r))
///
/// `k = 60` is the standard value from Cormack et al. (2009), also used by
/// InfinityDB.  A larger k reduces the influence of absolute ranks (makes the
/// fusion softer); a smaller k emphasises the top positions of each list.
pub fn rrf_fusion(rankings: Vec<Vec<(u64, f32)>>, rrf_k: f32, limit: usize) -> Vec<(u64, f32)> {
    let mut scores: HashMap<u64, f32> = HashMap::new();

    for ranking in &rankings {
        for (rank, &(doc_id, _original_score)) in ranking.iter().enumerate() {
            *scores.entry(doc_id).or_insert(0.0) += 1.0 / (rrf_k + rank as f32 + 1.0);
        }
    }

    let mut out: Vec<(u64, f32)> = scores.into_iter().collect();
    out.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
    out.truncate(limit);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn doc_in_both_lists_ranks_higher() {
        let list_a = vec![(1u64, 1.0), (2, 0.9), (3, 0.8)];
        let list_b = vec![(2u64, 1.0), (4, 0.9), (5, 0.8)];
        let fused = rrf_fusion(vec![list_a, list_b], 60.0, 5);
        // Doc 2 appears in both lists so should be first.
        assert_eq!(fused[0].0, 2);
    }
}
