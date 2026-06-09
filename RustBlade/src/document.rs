use std::collections::HashMap;
use serde::{Deserialize, Serialize};

// ---------------------------------------------------------------------------
// Field value — a document field can hold text, a dense vector, or a number.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum FieldValue {
    Text(String),
    Vector(Vec<f32>),
    Integer(i64),
    Float(f64),
}

// ---------------------------------------------------------------------------
// Document — what the caller inserts into the engine.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Document {
    pub id: u64,
    pub fields: HashMap<String, FieldValue>,
}

impl Document {
    pub fn new(id: u64) -> Self {
        Self { id, fields: HashMap::new() }
    }

    pub fn with_text(mut self, field: &str, value: impl Into<String>) -> Self {
        self.fields.insert(field.to_string(), FieldValue::Text(value.into()));
        self
    }

    pub fn with_vector(mut self, field: &str, value: Vec<f32>) -> Self {
        self.fields.insert(field.to_string(), FieldValue::Vector(value));
        self
    }

    pub fn with_int(mut self, field: &str, value: i64) -> Self {
        self.fields.insert(field.to_string(), FieldValue::Integer(value));
        self
    }

    pub fn with_float(mut self, field: &str, value: f64) -> Self {
        self.fields.insert(field.to_string(), FieldValue::Float(value));
        self
    }

    /// Collect all text field values, used by the tokenizer pipeline.
    pub fn text_fields(&self) -> Vec<(&str, &str)> {
        self.fields.iter().filter_map(|(k, v)| {
            if let FieldValue::Text(t) = v { Some((k.as_str(), t.as_str())) } else { None }
        }).collect()
    }

    pub fn get_vector(&self, field: &str) -> Option<&Vec<f32>> {
        self.fields.get(field).and_then(|v| {
            if let FieldValue::Vector(vec) = v { Some(vec) } else { None }
        })
    }
}

// ---------------------------------------------------------------------------
// SearchResult — what the engine returns to the caller.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone)]
pub struct SearchResult {
    pub doc_id: u64,
    /// Higher is always better, regardless of search mode.
    pub score: f32,
    /// Stored text fields (populated from DocStore).
    pub fields: HashMap<String, String>,
}

impl SearchResult {
    pub fn new(doc_id: u64, score: f32) -> Self {
        Self { doc_id, score, fields: HashMap::new() }
    }
}
