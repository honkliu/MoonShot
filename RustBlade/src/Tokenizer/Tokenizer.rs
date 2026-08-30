#![allow(non_snake_case)]

pub trait Tokenizer: Send + Sync {
    fn Tokenize(&self, text: &str) -> Vec<String>;
}
