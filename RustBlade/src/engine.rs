use std::collections::HashMap;
use std::path::Path;

use serde::{Deserialize, Serialize};

use crate::doc_store::{DocStore, StoredDoc};
use crate::document::{Document, FieldValue, SearchResult};
use crate::error::{Result, RustBladeError};
use crate::executor::QueryExecutor;
use crate::inverted_index::{InvertedIndex, InvertedIndexBuilder, InvertedIndexSnapshot};
use crate::query::{QueryNode, QueryParser};
use crate::tokenizer::{SimpleTokenizer, Tokenizer};
use crate::vector_index::{Metric, VectorIndex};

// ---------------------------------------------------------------------------
// Persistence snapshot — must live at module level for #[derive] to work.
// ---------------------------------------------------------------------------
#[derive(Serialize, Deserialize)]
struct Snapshot {
    config:      EngineConfig,
    inv_index:   InvertedIndexSnapshot,
    vec_indexes: HashMap<String, VectorIndex>,
    doc_store:   DocStore,
}

// ---------------------------------------------------------------------------
// Engine configuration
// ---------------------------------------------------------------------------
#[derive(Clone, Serialize, Deserialize)]
pub struct EngineConfig {
    /// Which vector fields to index, and how.
    pub vector_fields: HashMap<String, VectorFieldConfig>,
    /// Default number of results returned by `search()`.
    pub default_limit: usize,
    /// ef parameter for HNSW search (controls recall vs speed).
    pub hnsw_ef_search: usize,
    /// RRF k constant used in hybrid search.
    pub rrf_k: f32,
    /// Text fields to index (empty = index all text fields).
    pub text_fields: Vec<String>,
}

#[derive(Clone, Serialize, Deserialize)]
pub struct VectorFieldConfig {
    pub dim:    usize,
    pub index:  VectorIndexType,
    pub metric: Metric,
}

#[derive(Clone, Serialize, Deserialize)]
pub enum VectorIndexType {
    Flat,
    Hnsw { m: usize, ef_construction: usize },
}

impl Default for EngineConfig {
    fn default() -> Self {
        Self {
            vector_fields: HashMap::new(),
            default_limit: 10,
            hnsw_ef_search: 64,
            rrf_k: 60.0,
            text_fields: vec![],
        }
    }
}

impl EngineConfig {
    /// Register a vector field using HNSW with sensible defaults.
    pub fn with_hnsw_field(mut self, field: &str, dim: usize, metric: Metric) -> Self {
        self.vector_fields.insert(field.to_string(), VectorFieldConfig {
            dim,
            index: VectorIndexType::Hnsw { m: 16, ef_construction: 200 },
            metric,
        });
        self
    }

    /// Register a vector field using brute-force Flat search.
    pub fn with_flat_field(mut self, field: &str, dim: usize, metric: Metric) -> Self {
        self.vector_fields.insert(field.to_string(), VectorFieldConfig {
            dim,
            index: VectorIndexType::Flat,
            metric,
        });
        self
    }
}

// ---------------------------------------------------------------------------
// Pending state — accumulated before build() is called.
// ---------------------------------------------------------------------------
struct PendingState {
    inv_builder:    InvertedIndexBuilder,
    vec_builders:   HashMap<String, VectorIndex>,
}

// ---------------------------------------------------------------------------
// Engine — the main entry point for RustBlade.
//
// Lifecycle:
//   1. Engine::new(config)           — configure
//   2. engine.add_document(doc)      — index documents
//   3. engine.build()                — compile indexes
//   4. engine.search*(...)           — query
//   5. engine.save(path)             — persist
//   6. Engine::load(path)            — restore
// ---------------------------------------------------------------------------
pub struct Engine {
    config:     EngineConfig,
    tokenizer:  Box<dyn Tokenizer>,
    doc_store:  DocStore,
    // Set after build().
    inv_index:  Option<InvertedIndex>,
    vec_indexes: HashMap<String, VectorIndex>,
    // Accumulated before build().
    pending:    Option<PendingState>,
}

impl Engine {
    // -- construction -------------------------------------------------------

    pub fn new() -> Self {
        Self::with_config(EngineConfig::default())
    }

    pub fn with_config(config: EngineConfig) -> Self {
        let mut vec_builders = HashMap::new();
        for (field, cfg) in &config.vector_fields {
            let idx = match &cfg.index {
                VectorIndexType::Flat => VectorIndex::flat(cfg.dim, cfg.metric),
                VectorIndexType::Hnsw { m, ef_construction } => {
                    VectorIndex::hnsw_custom(cfg.dim, *m, *ef_construction, cfg.metric)
                }
            };
            vec_builders.insert(field.clone(), idx);
        }

        Self {
            config,
            tokenizer:   Box::new(SimpleTokenizer),
            doc_store:   DocStore::new(),
            inv_index:   None,
            vec_indexes: HashMap::new(),
            pending: Some(PendingState {
                inv_builder:  InvertedIndexBuilder::new(),
                vec_builders,
            }),
        }
    }

    /// Replace the default SimpleTokenizer with a custom one.
    pub fn with_tokenizer(mut self, tok: Box<dyn Tokenizer>) -> Self {
        self.tokenizer = tok;
        self
    }

    // -- indexing -----------------------------------------------------------

    pub fn add_document(&mut self, doc: Document) -> Result<()> {
        let pending = self.pending.as_mut().ok_or(RustBladeError::IndexNotBuilt)?;

        // 1. Collect tokens from all (or configured) text fields.
        let mut all_tokens: Vec<String> = Vec::new();
        let text_fields = doc.text_fields();

        for (field, text) in &text_fields {
            if self.config.text_fields.is_empty()
                || self.config.text_fields.iter().any(|f| f == field)
            {
                let tokens = self.tokenizer.tokenize(text);
                all_tokens.extend(tokens);
            }
        }

        if !all_tokens.is_empty() {
            pending.inv_builder.add_document(doc.id, all_tokens);
        }

        // 2. Add vectors to the appropriate VectorIndex builder.
        for (field, _cfg) in &self.config.vector_fields {
            if let Some(vec) = doc.get_vector(field) {
                let dim = vec.len();
                let entry = pending.vec_builders.entry(field.clone()).or_insert_with(|| {
                    // Auto-create a flat index if not pre-configured.
                    VectorIndex::flat(dim, Metric::Cosine)
                });
                let idx_dim = entry.dim();
                if idx_dim > 0 && idx_dim != dim {
                    return Err(RustBladeError::DimensionMismatch { expected: idx_dim, got: dim });
                }
                entry.add(doc.id, vec.clone());
            }
        }

        // Also handle vector fields not in config (auto-register them).
        for (field, value) in &doc.fields {
            if let FieldValue::Vector(vec) = value {
                if !self.config.vector_fields.contains_key(field) {
                    let entry = pending.vec_builders.entry(field.clone())
                        .or_insert_with(|| VectorIndex::hnsw(vec.len(), Metric::Cosine));
                    entry.add(doc.id, vec.clone());
                }
            }
        }

        // 3. Store text fields in DocStore.
        let mut stored = StoredDoc::default();
        for (field, text) in text_fields {
            stored.text_fields.insert(field.to_string(), text.to_string());
        }
        self.doc_store.add(doc.id, stored);

        Ok(())
    }

    // -- build --------------------------------------------------------------

    /// Compile all pending documents into searchable indexes.
    pub fn build(&mut self) -> Result<()> {
        let pending = self.pending.take().ok_or(RustBladeError::IndexNotBuilt)?;

        self.inv_index   = Some(pending.inv_builder.build());
        self.vec_indexes = pending.vec_builders;

        Ok(())
    }

    // -- search -------------------------------------------------------------

    /// Full-text search using BM25 over the inverted index.
    pub fn search(&mut self, query_str: &str, limit: usize) -> Result<Vec<SearchResult>> {
        let inv = self.inv_index.as_mut().ok_or(RustBladeError::IndexNotBuilt)?;
        let tok: &dyn Tokenizer = &*self.tokenizer;
        let parser = QueryParser::new(tok);
        let node   = parser.parse(query_str);

        let mut executor = QueryExecutor::new(inv, &self.vec_indexes);
        let mut results  = executor.execute(&node, limit);
        self.enrich_results(&mut results);
        Ok(results)
    }

    /// Vector (KNN) search on `field`.
    pub fn search_knn(&mut self, field: &str, vector: &[f32], k: usize) -> Result<Vec<SearchResult>> {
        let inv = self.inv_index.as_mut().ok_or(RustBladeError::IndexNotBuilt)?;
        let query = QueryNode::Knn {
            field:  field.to_string(),
            vector: vector.to_vec(),
            k,
        };
        let mut executor = QueryExecutor::new(inv, &self.vec_indexes);
        let mut results  = executor.execute(&query, k);
        self.enrich_results(&mut results);
        Ok(results)
    }

    /// Hybrid search: full-text + KNN fused with RRF.
    pub fn search_hybrid(
        &mut self,
        query_str:  &str,
        field:      &str,
        vector:     &[f32],
        limit:      usize,
    ) -> Result<Vec<SearchResult>> {
        let inv = self.inv_index.as_mut().ok_or(RustBladeError::IndexNotBuilt)?;
        let tok: &dyn Tokenizer = &*self.tokenizer;
        let parser = QueryParser::new(tok);
        let text   = parser.parse(query_str);

        let rrf_k = self.config.rrf_k;
        let query = QueryNode::Hybrid {
            text:   Box::new(text),
            vector: vector.to_vec(),
            field:  field.to_string(),
            k:      limit,
            rrf_k,
        };

        let mut executor = QueryExecutor::new(inv, &self.vec_indexes);
        let mut results  = executor.execute(&query, limit);
        self.enrich_results(&mut results);
        Ok(results)
    }

    /// Execute a pre-built QueryNode directly (advanced API).
    pub fn execute_query(&mut self, node: &QueryNode, limit: usize) -> Result<Vec<SearchResult>> {
        let inv = self.inv_index.as_mut().ok_or(RustBladeError::IndexNotBuilt)?;
        let mut executor = QueryExecutor::new(inv, &self.vec_indexes);
        let mut results  = executor.execute(node, limit);
        self.enrich_results(&mut results);
        Ok(results)
    }

    // -- stats --------------------------------------------------------------

    pub fn doc_count(&self) -> usize {
        self.doc_store.len()
    }

    pub fn is_built(&self) -> bool {
        self.inv_index.is_some()
    }

    // -- persistence --------------------------------------------------------

    pub fn save(&self, path: impl AsRef<Path>) -> Result<()> {
        let inv = self.inv_index.as_ref().ok_or(RustBladeError::IndexNotBuilt)?;
        let doc_store_bytes = serde_json::to_vec(&self.doc_store)?;
        let vec_bytes = serde_json::to_vec(&self.vec_indexes)?;
        let snap = Snapshot {
            config:      self.config.clone(),
            inv_index:   InvertedIndexSnapshot::from(inv),
            vec_indexes: serde_json::from_slice(&vec_bytes)?,
            doc_store:   serde_json::from_slice(&doc_store_bytes)?,
        };
        let bytes = serde_json::to_vec_pretty(&snap)?;
        std::fs::write(path, bytes)?;
        Ok(())
    }

    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let bytes = std::fs::read(path)?;
        let snap: Snapshot = serde_json::from_slice(&bytes)?;
        let mut engine = Engine::with_config(snap.config);
        engine.inv_index   = Some(InvertedIndex::from(snap.inv_index));
        engine.vec_indexes = snap.vec_indexes;
        engine.doc_store   = snap.doc_store;
        engine.pending     = None;  // already built
        Ok(engine)
    }

    // -- private ------------------------------------------------------------

    fn enrich_results(&self, results: &mut Vec<SearchResult>) {
        for r in results.iter_mut() {
            self.doc_store.enrich(r.doc_id, &mut r.fields);
        }
    }
}

impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}
