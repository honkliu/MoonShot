// ---------------------------------------------------------------------------
// RustBlade — public API surface
//
// Design map:
//   MoonShot concept       → RustBlade equivalent
//   ─────────────────────────────────────────────
//   IndexContext            → Engine
//   IndexReader / ISR       → isr::Isr trait
//   AdvancedIndexReader     → isr::InvertedIsr
//   AndIsr / OrIsr          → isr::{AndIsr, OrIsr, NotIsr}
//   BlockTable / BlockCache → clock_cache::ClockCache
//   SimpleISRPool (REF)     → isr_pool::IsrPool
//   IndexSearchCompiler     → query::{QueryNode, QueryParser}
//   IndexSearchExecutor     → executor::QueryExecutor
//   UnifiedDecoder          → codec::{decode, decode_postings}
//   AdvancedIndexWriter     → inverted_index::InvertedIndexBuilder
//   Embeddings<T>           → vector_index::VectorIndex (Flat + HNSW)
//   SmartTokenizer          → tokenizer::SimpleTokenizer
//   BM25 scorer (missing)   → bm25::Bm25Scorer
//   RRF fusion (missing)    → fusion::rrf_fusion
// ---------------------------------------------------------------------------

pub mod error;
pub mod document;
pub mod tokenizer;
pub mod codec;
pub mod postings;
pub mod dictionary;
pub mod isr;
pub mod isr_pool;
pub mod clock_cache;
pub mod bm25;
pub mod inverted_index;
pub mod vector_index;
pub mod query;
pub mod executor;
pub mod doc_store;
pub mod fusion;
pub mod engine;

// -- Flat re-exports for the happy path ------------------------------------
pub use engine::{Engine, EngineConfig, VectorFieldConfig, VectorIndexType};
pub use document::{Document, FieldValue, SearchResult};
pub use vector_index::Metric;
pub use query::{QueryNode, QueryParser};
pub use tokenizer::{SimpleTokenizer, Tokenizer, WhitespaceTokenizer};
pub use error::{Result, RustBladeError};
