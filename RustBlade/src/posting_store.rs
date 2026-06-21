use std::collections::HashMap;

use crate::block_table::DOC_VECTOR_DIM;

#[derive(Clone, Debug)]
pub struct IndexEntry {
    pub ie_doc_id:          u64,
    pub ie_term_frequency:  u32,
}

/* PostingList — sorted entries. get_bytes() encodes the current entries. */
#[derive(Clone, Debug, Default)]
pub struct PostingList {
    pub entries: Vec<IndexEntry>,
}

impl PostingList {
    pub fn doc_freq(&self) -> u32 { self.entries.len() as u32 }

    pub fn get_bytes(&self) -> Vec<u8> {
        self.encode()
    }

    fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.entries.len() * 3);
        for e in &self.entries {
            vb_write(e.ie_doc_id, &mut out);
            vb_write(e.ie_term_frequency as u64, &mut out);
        }
        out
    }
}

fn vb_write(mut v: u64, out: &mut Vec<u8>) {
    loop {
        if v < 0x80 { out.push(v as u8); break; }
        out.push(((v & 0x7F) | 0x80) as u8);
        v >>= 7;
    }
}

#[derive(Clone, Debug, Default)]
pub struct DocStats {
    pub doc_len:    u32,
    pub importance: f32,
    pub path:       String,
}

#[derive(Debug, Default)]
pub struct PostingStore {
    postings:    HashMap<String, PostingList>,
    doc_stats:   HashMap<u64, DocStats>,
    doc_vectors: HashMap<u64, [i8; DOC_VECTOR_DIM]>,
    total_terms: u64,
}

impl PostingStore {
    pub fn new() -> Self { Self::default() }

    #[allow(non_snake_case)]
    pub fn AddPosting(&mut self, stream_key: &str, doc_id: u64, tf: u32) {
        let pl = self.postings.entry(stream_key.to_string()).or_default();
        match pl.entries.binary_search_by_key(&doc_id, |e| e.ie_doc_id) {
            Ok(i)  => pl.entries[i].ie_term_frequency = tf,
            Err(i) => pl.entries.insert(i, IndexEntry { ie_doc_id: doc_id, ie_term_frequency: tf }),
        }
    }

    #[allow(non_snake_case)]
    pub fn AddDocTokens(&mut self, doc_id: u64, count: u32) {
        self.doc_stats.entry(doc_id).or_default().doc_len += count;
        self.total_terms += count as u64;
    }

    #[allow(non_snake_case)]
    pub fn SetDocImportance(&mut self, doc_id: u64, score: f32) {
        self.doc_stats.entry(doc_id).or_default().importance = score;
    }

    #[allow(non_snake_case)]
    pub fn SetDocPath(&mut self, doc_id: u64, path: String) {
        self.doc_stats.entry(doc_id).or_default().path = path;
    }

    #[allow(non_snake_case)]
    pub fn SetDocVector(&mut self, doc_id: u64, vector: Vec<f32>) -> bool {
        if vector.len() != DOC_VECTOR_DIM { return false; }
        self.doc_stats.entry(doc_id).or_default();
        self.doc_vectors.insert(doc_id, quantize_vector(&vector));
        true
    }

    #[allow(non_snake_case)]
    pub fn SetDocVectorBytes(&mut self, doc_id: u64, vector: &[u8]) -> bool {
        self.doc_stats.entry(doc_id).or_default();
        let mut bytes = [0i8; DOC_VECTOR_DIM];
        for i in 0..DOC_VECTOR_DIM {
            bytes[i] = vector[i] as i8;
        }
        self.doc_vectors.insert(doc_id, bytes);
        true
    }

    #[allow(non_snake_case)]
    pub fn GetDocVector(&self, doc_id: u64) -> &[i8; DOC_VECTOR_DIM] {
        self.doc_vectors.get(&doc_id).expect("fixed DocData vector is required")
    }

    #[allow(non_snake_case)]
    pub fn GetDocPath(&self, doc_id: u64) -> &str {
        self.doc_stats.get(&doc_id).map(|s| s.path.as_str()).unwrap_or("")
    }

    #[allow(non_snake_case)]
    pub fn GetPostingList(&self, key: &str) -> Option<&PostingList> {
        self.postings.get(key)
    }

    #[allow(non_snake_case)]
    pub fn GetDocLen(&self, doc_id: u64) -> u32 {
        self.doc_stats.get(&doc_id).map(|s| s.doc_len).unwrap_or(1)
    }

    #[allow(non_snake_case)]
    pub fn GetDocImportance(&self, doc_id: u64) -> f32 {
        self.doc_stats.get(&doc_id).map(|s| s.importance).unwrap_or(0.0)
    }

    #[allow(non_snake_case)]
    pub fn TotalDocs(&self)  -> u64 { self.doc_stats.len() as u64 }
    #[allow(non_snake_case)]
    pub fn TotalTerms(&self) -> u64 { self.total_terms }

    #[allow(non_snake_case)]
    pub fn AvgDocLen(&self) -> f32 {
        if self.doc_stats.is_empty() { return 1.0; }
        self.total_terms as f32 / self.doc_stats.len() as f32
    }

    #[allow(non_snake_case)]
    pub fn DocFreq(&self, key: &str) -> u32 {
        self.postings.get(key).map(|pl| pl.doc_freq()).unwrap_or(0)
    }

    #[allow(non_snake_case)]
    pub fn AllPostings(&self) -> &HashMap<String, PostingList> {
        &self.postings
    }

    #[allow(non_snake_case)]
    pub fn AllPostingsMut(&mut self) -> &mut HashMap<String, PostingList> {
        &mut self.postings
    }

    #[allow(non_snake_case)]
    pub fn AllDocStats(&self) -> &HashMap<u64, DocStats> {
        &self.doc_stats
    }
}

fn quantize_vector(vector: &[f32]) -> [i8; DOC_VECTOR_DIM] {
    let mut out = [0i8; DOC_VECTOR_DIM];
    for i in 0..DOC_VECTOR_DIM {
        let clipped = (vector[i] * 128.0).clamp(-128.0, 127.0);
        out[i] = clipped.round() as i8;
    }
    out
}
