use std::collections::HashMap;
use serde::{Deserialize, Serialize};

// ---------------------------------------------------------------------------
// DocStore — maps doc_id → stored fields for display / snippet generation.
//
// Analogous to REF's GetDocData() / InfinityDB's column store.
// Only text fields are stored here; vector fields are in VectorIndex.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct StoredDoc {
    /// Text fields (field_name → text).
    pub text_fields: HashMap<String, String>,
    /// Arbitrary string metadata (e.g. file path, URL, timestamp).
    pub meta: HashMap<String, String>,
}

impl StoredDoc {
    pub fn get_field(&self, field: &str) -> Option<&str> {
        self.text_fields.get(field).map(String::as_str)
    }

    pub fn get_meta(&self, key: &str) -> Option<&str> {
        self.meta.get(key).map(String::as_str)
    }
}

#[derive(Default, Serialize, Deserialize)]
pub struct DocStore {
    docs: HashMap<u64, StoredDoc>,
}

impl DocStore {
    pub fn new() -> Self {
        Default::default()
    }

    pub fn add(&mut self, doc_id: u64, doc: StoredDoc) {
        self.docs.insert(doc_id, doc);
    }

    pub fn get(&self, doc_id: u64) -> Option<&StoredDoc> {
        self.docs.get(&doc_id)
    }

    pub fn get_text_field(&self, doc_id: u64, field: &str) -> Option<&str> {
        self.docs.get(&doc_id)?.get_field(field)
    }

    /// Copy the stored text fields into `result.fields` for display.
    pub fn enrich(&self, doc_id: u64, fields: &mut HashMap<String, String>) {
        if let Some(doc) = self.docs.get(&doc_id) {
            for (k, v) in &doc.text_fields {
                fields.entry(k.clone()).or_insert_with(|| v.clone());
            }
        }
    }

    pub fn len(&self) -> usize { self.docs.len() }
    pub fn is_empty(&self) -> bool { self.docs.is_empty() }
}
