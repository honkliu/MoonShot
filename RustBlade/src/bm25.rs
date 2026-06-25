/*
* Bm25Scorer — Okapi BM25 relevance scoring.
*
*   IDF(t)       = ln( (N - df + 0.5) / (df + 0.5) + 1 )
*   TF_norm(t,d) = tf * (k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
*   score(t,d)   = IDF(t) * TF_norm(t,d)
*
* Default parameters: k1 = 1.2, b = 0.75  (Okapi standard)
*/
#[allow(non_snake_case)]
pub struct Bm25Scorer {
    m_K1:     f32,
    m_B:      f32,
    m_TotalDocs:      f32,
    m_AverageDocLength: f32,
}

#[allow(non_snake_case)]
impl Bm25Scorer {
    pub fn new(num_docs: u64, avg_doc_len: f32) -> Self {
        Self::with_params(num_docs, avg_doc_len, 1.2, 0.75)
    }

    pub fn with_params(num_docs: u64, avg_doc_len: f32, k1: f32, b: f32) -> Self {
        Self {
            m_K1: k1,
            m_B: b,
            m_TotalDocs:      num_docs.max(1) as f32,
            m_AverageDocLength: avg_doc_len.max(1.0),
        }
    }

    pub fn Idf(&self, docFreq: u32) -> f32 {
        let df = docFreq.max(1) as f32;
        ((self.m_TotalDocs - df + 0.5) / (df + 0.5) + 1.0).ln().max(0.0)
    }

    pub fn TfNorm(&self, tf: u32, docLen: u32) -> f32 {
        let f  = tf as f32;
        let dl = docLen.max(1) as f32;
        f * (self.m_K1 + 1.0) / (f + self.m_K1 * (1.0 - self.m_B + self.m_B * dl / self.m_AverageDocLength))
    }

    pub fn score(&self, tf: u32, doc_len: u32, doc_freq: u32) -> f32 {
        self.Idf(doc_freq) * self.TfNorm(tf, doc_len)
    }
}
