/*
* Bm25Scorer — Okapi BM25 relevance scoring.
*
*   IDF(t)       = ln( (N - df + 0.5) / (df + 0.5) + 1 )
*   TF_norm(t,d) = tf * (k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
*   score(t,d)   = IDF(t) * TF_norm(t,d)
*
* Default parameters: k1 = 1.2, b = 0.75  (Okapi standard)
*/
pub struct Bm25Scorer {
    k1:     f32,
    b:      f32,
    n:      f32,
    avg_dl: f32,
}

impl Bm25Scorer {
    pub fn new(num_docs: u64, avg_doc_len: f32) -> Self {
        Self::with_params(num_docs, avg_doc_len, 1.2, 0.75)
    }

    pub fn with_params(num_docs: u64, avg_doc_len: f32, k1: f32, b: f32) -> Self {
        Self {
            k1,
            b,
            n:      num_docs.max(1) as f32,
            avg_dl: avg_doc_len.max(1.0),
        }
    }

    pub fn idf(&self, doc_freq: u32) -> f32 {
        let df = doc_freq.max(1) as f32;
        ((self.n - df + 0.5) / (df + 0.5) + 1.0).ln().max(0.0)
    }

    pub fn tf_norm(&self, tf: u32, doc_len: u32) -> f32 {
        let f  = tf as f32;
        let dl = doc_len.max(1) as f32;
        f * (self.k1 + 1.0) / (f + self.k1 * (1.0 - self.b + self.b * dl / self.avg_dl))
    }

    pub fn score(&self, tf: u32, doc_len: u32, doc_freq: u32) -> f32 {
        self.idf(doc_freq) * self.tf_norm(tf, doc_len)
    }
}
