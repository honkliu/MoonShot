//! RustBlade public surface; MoonShot API names intentionally retain C++ parity.
#![allow(non_snake_case, non_upper_case_globals)]

#[cfg(not(target_arch = "wasm32"))]
use mimalloc::MiMalloc;

#[cfg(not(target_arch = "wasm32"))]
#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

#[path = "IndexAccess/AdvancedIndexReader.rs"]
pub mod advanced_index_reader;
#[path = "IndexAccess/AdvancedIndexWriter.rs"]
pub mod advanced_index_writer;
#[path = "IndexAccess/BlockTable.rs"]
pub mod block_table;
#[path = "Configuration/ConfigParameters.rs"]
pub mod config_parameters;
#[path = "Utils/Constants.rs"]
pub mod constants;
#[path = "IndexAccess/ElementFilter.rs"]
pub mod element_filter;
#[path = "Embeddings/Embeddings.rs"]
pub mod embeddings;
#[path = "Rust/Error.rs"]
pub mod error;
#[path = "Compiler/EvalExpression.rs"]
pub mod eval_expression;
#[path = "Utils/FileAccess.rs"]
pub mod file_access;
#[path = "IndexAccess/FileBlockManager.rs"]
pub mod file_block_manager;
#[path = "IndexAccess/HashFunctions.rs"]
pub mod hash_functions;
#[path = "IndexAccess/IndexContext.rs"]
pub mod index_context;
#[path = "IndexAccess/IndexReader.rs"]
pub mod index_reader;
#[path = "IndexAccess/IndexReaderImpl.rs"]
pub mod index_reader_impl;
#[path = "Compiler/IndexSearchCompiler.rs"]
pub mod index_search_compiler;
#[path = "Executor/IndexSearchExecutor.rs"]
pub mod index_search_executor;
#[path = "IndexAccess/IndexSerializer.rs"]
pub mod index_serializer;
#[path = "IndexAccess/IndexWriter.rs"]
pub mod index_writer;
#[path = "IndexAccess/MemOperation.rs"]
pub mod mem_operation;
#[path = "IndexAccess/PostingStore.rs"]
pub mod posting_store;
#[path = "IndexAccess/SearchResult.rs"]
pub mod search_result;
#[path = "Tokenizer/SmartTokenizer.rs"]
pub mod tokenizer;
#[path = "Tokenizer/Tokenizer.rs"]
pub mod tokenizer_interface;
#[path = "IndexAccess/UnifiedDecoder.rs"]
pub mod unified_decoder;

#[cfg(feature = "wasm")]
#[path = "Tools/moon_wasm/WasmApi.rs"]
pub mod wasm_api;

pub use embeddings::{
    FreshDiskAnnVectorIndex, IEmbeddingModel, Node, TFIDFSemanticEmbedding, VectorMetric,
    VectorSearchResult,
};
pub use error::{Result, RustBladeError};
pub use eval_expression::{
    kWeakAndBigramBoostForDocParameters, kWeakAndBigramBoostParameters, kWeakAndBigramParameters,
    EvalNode, EvalTree, GetQueryCompileModeParameters, QueryCompileMode,
    QueryCompileModeParameters,
};
pub use index_context::{Document, IndexContext, SearchTask};
pub use index_reader::IndexReader;
pub use index_writer::IndexWriter;
pub use search_result::SearchResult;
pub use tokenizer::SmartTokenizer;
