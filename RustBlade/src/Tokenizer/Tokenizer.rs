//! Direct translation of the C++ tokenizer contract; names stay aligned for API parity.
#![allow(non_snake_case, non_upper_case_globals)]

pub trait Tokenizer: Send + Sync {
    fn Tokenize(&self, text: &str) -> Vec<String>;
}
