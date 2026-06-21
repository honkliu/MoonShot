use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use crate::posting_store::PostingStore;
use crate::eval_tree::BIGRAM_SEP;

pub trait IndexWriter {
    #[allow(non_snake_case)]
    fn Write(&mut self, tokens: Vec<String>, doc_id: u64, stream: &str);
    #[allow(non_snake_case)]
    fn SetDocImportance(&mut self, doc_id: u64, score: f32);
    #[allow(non_snake_case)]
    fn SetDocPath(&mut self, doc_id: u64, path: String);
    #[allow(non_snake_case)]
    fn SetDocVector(&mut self, doc_id: u64, vector: Vec<f32>);
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
    fn Write(&mut self, tokens: Vec<String>, doc_id: u64, stream: &str) {
        if tokens.is_empty() { return; }
        let abbrev = Self::stream_abbrev(stream);
        let mut store = self.store.lock().unwrap();

        let mut term_tf: HashMap<String, u32> = HashMap::new();
        for tok in &tokens {
            if !tok.is_empty() {
                *term_tf.entry(tok.clone()).or_insert(0) += 1;
            }
        }

        /* bigrams: "race_car" etc. */
        for i in 0..tokens.len().saturating_sub(1) {
            if !tokens[i].is_empty() && !tokens[i + 1].is_empty() {
                let bigram = format!("{}{}{}", tokens[i], BIGRAM_SEP, tokens[i + 1]);
                *term_tf.entry(bigram).or_insert(0) += 1;
            }
        }

        for (term, tf) in &term_tf {
            store.AddPosting(&format!("{}{}", term, abbrev), doc_id, *tf);
        }

        store.AddDocTokens(doc_id, tokens.len() as u32);
    }

    fn SetDocImportance(&mut self, doc_id: u64, score: f32) {
        self.store.lock().unwrap().SetDocImportance(doc_id, score);
    }

    fn SetDocPath(&mut self, doc_id: u64, path: String) {
        self.store.lock().unwrap().SetDocPath(doc_id, path);
    }

    fn SetDocVector(&mut self, doc_id: u64, vector: Vec<f32>) {
        self.store.lock().unwrap().SetDocVector(doc_id, vector);
    }
}
