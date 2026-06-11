use thiserror::Error;

#[derive(Error, Debug)]
pub enum RustBladeError {
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Serialization error: {0}")]
    Serde(#[from] serde_json::Error),

    #[error("Invalid index format")]
    InvalidFormat,

    #[error("Index not built — call build() first")]
    IndexNotBuilt,

    #[error("Document {0} not found")]
    DocumentNotFound(u64),

    #[error("Vector dimension mismatch: expected {expected}, got {got}")]
    DimensionMismatch { expected: usize, got: usize },

    #[error("Empty index")]
    EmptyIndex,

    #[error("Invalid query: {0}")]
    InvalidQuery(String),
}

pub type Result<T> = std::result::Result<T, RustBladeError>;
