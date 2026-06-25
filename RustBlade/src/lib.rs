#[cfg(not(target_arch = "wasm32"))]
use mimalloc::MiMalloc;

#[cfg(not(target_arch = "wasm32"))]
#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

pub mod error;
pub mod tokenizer;
pub mod pinned_memory;
pub mod posting_store;
pub mod block_table;
pub mod varbyte_decoder;
pub mod index_writer;
pub mod eval_tree;
pub mod bm25;
pub mod index_reader;
pub mod advanced_reader;
pub mod composite_readers;
pub mod compiler;
pub mod executor;
pub mod serializer;
pub mod index_context;
pub mod vector_index;
pub mod fusion;

#[cfg(feature = "wasm")]
pub mod wasm_api;

pub use error::{RustBladeError, Result};
pub use tokenizer::SmartTokenizer;
pub use index_writer::IndexWriter;
pub use eval_tree::{EvalTree, EvalNode};
pub use index_reader::IndexReader;
pub use executor::SearchResult;
pub use index_context::{Document, IndexContext};
pub use vector_index::{HnswIndex, Metric, VectorMetric, VectorSearchResult};
pub use fusion::rrf_fusion as rrf_merge;
