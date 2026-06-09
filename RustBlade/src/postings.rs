use serde::{Deserialize, Serialize};
use crate::codec;

// ---------------------------------------------------------------------------
// PostingEntry — a single (doc_id, term_freq) pair in a posting list.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct PostingEntry {
    pub doc_id: u64,
    pub term_freq: u32,
}

// ---------------------------------------------------------------------------
// PostingListBuilder — accumulate entries then call `build()`.
// Entries MUST be added in ascending doc_id order.
// ---------------------------------------------------------------------------
#[derive(Default)]
pub struct PostingListBuilder {
    entries: Vec<PostingEntry>,
}

impl PostingListBuilder {
    pub fn new() -> Self {
        Default::default()
    }

    pub fn push(&mut self, doc_id: u64, tf: u32) {
        if let Some(last) = self.entries.last() {
            debug_assert!(doc_id >= last.doc_id, "doc_ids must be non-decreasing");
        }
        self.entries.push(PostingEntry { doc_id, term_freq: tf });
    }

    pub fn build(self) -> PostingList {
        let raw: Vec<(u64, u32)> = self.entries.iter().map(|e| (e.doc_id, e.term_freq)).collect();
        let bytes = codec::encode_postings(&raw);
        PostingList {
            bytes,
            doc_freq: self.entries.len() as u32,
            decoded: self.entries,
        }
    }
}

// ---------------------------------------------------------------------------
// PostingList — immutable, stored in the TermDictionary.
//
// Both the raw VarByte bytes (compact, disk-friendly) and the decoded Vec
// (fast random access for BM25 and the clock cache) are kept.
// ---------------------------------------------------------------------------
#[derive(Clone, Serialize, Deserialize)]
pub struct PostingList {
    /// VarByte-delta-compressed bytes — used when persisting to disk.
    pub bytes: Vec<u8>,
    /// Number of documents that contain this term.
    pub doc_freq: u32,
    /// Decoded entries — accessed at query time via the ISR pool.
    pub decoded: Vec<PostingEntry>,
}

impl PostingList {
    /// Deserialize from the compact byte form (e.g. when loading from disk).
    pub fn from_bytes(bytes: Vec<u8>) -> Self {
        let pairs = codec::decode_postings(&bytes);
        let doc_freq = pairs.len() as u32;
        let decoded = pairs.into_iter()
            .map(|(doc_id, term_freq)| PostingEntry { doc_id, term_freq })
            .collect();
        PostingList { bytes, doc_freq, decoded }
    }
}
