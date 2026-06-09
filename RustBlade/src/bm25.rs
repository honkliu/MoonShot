/// BM25 relevance scorer.
///
/// Okapi BM25 with the standard Robertson–Spärck Jones IDF variant:
///
///   IDF(t) = ln((N - df + 0.5) / (df + 0.5) + 1)
///   TF_norm(t,d) = tf * (k1 + 1) / (tf + k1 * (1 - b + b * dl/avgdl))
///   BM25(t,d) = IDF(t) * TF_norm(t,d)
///
/// Default parameters: k1 = 1.2, b = 0.75 (widely-used production defaults).
#[derive(Clone)]
pub struct Bm25Scorer {
    k1:      f32,
    b:       f32,
    n:       f32,  // total number of documents
    avg_dl:  f32,  // average document length
}

impl Bm25Scorer {
    pub fn new(num_docs: u64, avg_doc_len: f32) -> Self {
        Self { k1: 1.2, b: 0.75, n: num_docs as f32, avg_dl: avg_doc_len }
    }

    pub fn with_params(num_docs: u64, avg_doc_len: f32, k1: f32, b: f32) -> Self {
        Self { k1, b, n: num_docs as f32, avg_dl: avg_doc_len }
    }

    /// Inverse document frequency (always ≥ 0).
    pub fn idf(&self, doc_freq: u32) -> f32 {
        let df = doc_freq.max(1) as f32;
        ((self.n - df + 0.5) / (df + 0.5) + 1.0).ln().max(0.0)
    }

    /// Normalized term frequency.
    pub fn tf_norm(&self, tf: u32, doc_len: u32) -> f32 {
        let tf  = tf as f32;
        let dl  = doc_len as f32;
        let avg = self.avg_dl.max(1.0);
        tf * (self.k1 + 1.0) / (tf + self.k1 * (1.0 - self.b + self.b * dl / avg))
    }

    /// Combined BM25 contribution for one term in one document.
    pub fn score(&self, tf: u32, doc_len: u32, doc_freq: u32) -> f32 {
        self.idf(doc_freq) * self.tf_norm(tf, doc_len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn idf_is_non_negative() {
        let s = Bm25Scorer::new(1000, 100.0);
        for df in [1u32, 10, 100, 999, 1000] {
            assert!(s.idf(df) >= 0.0, "idf({df}) = {}", s.idf(df));
        }
    }

    #[test]
    fn higher_tf_means_higher_score() {
        let s = Bm25Scorer::new(1000, 100.0);
        let low  = s.score(1, 100, 10);
        let high = s.score(5, 100, 10);
        assert!(high > low);
    }
}
