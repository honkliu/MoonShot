use std::sync::{Arc, RwLock};

use crate::eval_expression::BIGRAM_SEP;
use crate::index_writer::IndexWriter;
use crate::posting_store::{PostingStore, StableHashMap};

#[allow(non_snake_case)]
pub struct AdvancedIndexWriter {
	m_Store: Arc<RwLock<PostingStore>>,
}

#[allow(non_snake_case)]
impl AdvancedIndexWriter {
	pub fn new(store: Arc<RwLock<PostingStore>>) -> Self {
		Self { m_Store: store }
	}

	pub fn SetDocPath(&mut self, doc_id: u64, path: String) {
		self.m_Store.write().unwrap().SetDocPath(doc_id, path);
	}

	fn StreamAbbrev(stream: &str) -> &'static str {
		if stream.is_empty() { return "B"; }
		match stream.as_bytes()[0] {
			b'A' | b'a' => "A",
			b'U' | b'u' => "U",
			b'T' | b't' => "T",
			b'B' | b'b' => "B",
			b'M' | b'm' => "M",
			_ => match stream.to_ascii_lowercase().as_str() {
				"title" => "T",
				"body" => "B",
				"anchor" => "A",
				"url" => "U",
				"meta" => "M",
				_ => "B",
			},
		}
	}
}

#[allow(non_snake_case)]
impl IndexWriter for AdvancedIndexWriter {
	fn Write(&mut self, words: Vec<String>, documentId: u64, postingType: &str) {
		if words.is_empty() { return; }

		let abbrev = Self::StreamAbbrev(postingType);
		let mut termTf = StableHashMap::<String, u32>::with_capacity_and_hasher(words.len() * 2, Default::default());
		for word in &words {
			if !word.is_empty() {
				*termTf.entry(word.clone()).or_insert(0) += 1;
			}
		}
		let uniqueUnigramCount = termTf.len() as u32;

		for pair in words.windows(2) {
			if !pair[0].is_empty() && !pair[1].is_empty() {
				let bigram = format!("{}{}{}", pair[0], BIGRAM_SEP, pair[1]);
				*termTf.entry(bigram).or_insert(0) += 1;
			}
		}

		let mut store = self.m_Store.write().unwrap();
		for (term, tf) in termTf {
			store.AddPosting(&format!("{term}{abbrev}"), documentId, tf);
		}
		store.AddDocTokens(documentId, words.len() as u32);
		store.AddStreamStats(documentId, abbrev.as_bytes()[0] as char, words.len() as u32, uniqueUnigramCount);
	}

	fn SetDocImportance(&mut self, doc_id: u64, score: f32) {
		self.m_Store.write().unwrap().SetDocImportance(doc_id, score);
	}

	fn SetDocVector(&mut self, doc_id: u64, vector: Vec<f32>) {
		self.m_Store.write().unwrap().SetDocVector(doc_id, vector);
	}
}
