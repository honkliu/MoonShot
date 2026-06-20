use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use crate::posting_store::PostingStore;
use crate::eval_tree::BIGRAM_SEP;

pub trait IndexWriter {
    fn write(&mut self, tokens: Vec<String>, doc_id: u64, stream: &str);
    fn set_doc_importance(&mut self, doc_id: u64, score: f32);
    fn set_doc_path(&mut self, doc_id: u64, path: String);
}

/*
* AdvancedIndexWriter — indexes unigrams and adjacent bigrams.
*
* For "good morning":
*   unigrams: goodT, morningT
*   bigrams:  good_morningT
*
* Bigrams score higher than scattered unigrams in BM25 (higher TF contribution).
* Mirrors MoonShot's AdvancedIndexWriter.
*/
pub struct AdvancedIndexWriter {
    store: Arc<Mutex<PostingStore>>,
}

impl AdvancedIndexWriter {
    pub fn new(store: Arc<Mutex<PostingStore>>) -> Self {
        Self { store }
    }

    fn stream_abbrev(stream: &str) -> &'static str {
        match stream.chars().next().map(|c| c.to_ascii_uppercase()) {
            Some('A') => "A",
            Some('U') => "U",
            Some('T') => "T",
            Some('B') => "B",
            Some('M') => "M",
            _ => match stream.to_lowercase().as_str() {
                "title"  => "T",
                "body"   => "B",
                "anchor" => "A",
                "url"    => "U",
                "meta"   => "M",
                _        => "B",
            },
        }
    }
}

impl IndexWriter for AdvancedIndexWriter {
    fn write(&mut self, tokens: Vec<String>, doc_id: u64, stream: &str) {
        if tokens.is_empty() { return; }
        let abbrev = Self::stream_abbrev(stream);
        let mut store = self.store.lock().unwrap();

        /* unigrams */
        let mut unigram_tf: HashMap<String, u32> = HashMap::new();
        for tok in &tokens {
            if !tok.is_empty() {
                *unigram_tf.entry(tok.clone()).or_insert(0) += 1;
            }
        }
        for (term, tf) in &unigram_tf {
            store.add_posting(&format!("{}{}", term, abbrev), doc_id, *tf);
        }

        /* bigrams: "race_car" etc. */
        let mut bigram_tf: HashMap<String, u32> = HashMap::new();
        for i in 0..tokens.len().saturating_sub(1) {
            if !tokens[i].is_empty() && !tokens[i + 1].is_empty() {
                let bigram = format!("{}{}{}", tokens[i], BIGRAM_SEP, tokens[i + 1]);
                *bigram_tf.entry(bigram).or_insert(0) += 1;
            }
        }
        for (bigram, tf) in &bigram_tf {
            store.add_posting(&format!("{}{}", bigram, abbrev), doc_id, *tf);
        }

        store.add_doc_tokens(doc_id, tokens.len() as u32);
    }

    fn set_doc_importance(&mut self, doc_id: u64, score: f32) {
        self.store.lock().unwrap().set_doc_importance(doc_id, score);
    }

    fn set_doc_path(&mut self, doc_id: u64, path: String) {
        self.store.lock().unwrap().set_doc_path(doc_id, path);
    }
}
