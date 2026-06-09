/// Tokenizer trait — converts raw text to a list of lowercase tokens.
///
/// Mirrors MoonShot's `Tokenizer` base class and REF's atom extraction logic.
/// The default implementation splits on Unicode word boundaries without
/// depending on ICU (Rust's `char::is_alphanumeric` is Unicode-aware).
pub trait Tokenizer: Send + Sync {
    fn tokenize(&self, text: &str) -> Vec<String>;
}

// ---------------------------------------------------------------------------
// SimpleTokenizer — splits on non-alphanumeric characters, lowercases.
// ---------------------------------------------------------------------------
pub struct SimpleTokenizer;

impl Tokenizer for SimpleTokenizer {
    fn tokenize(&self, text: &str) -> Vec<String> {
        text.to_lowercase()
            .split(|c: char| !c.is_alphanumeric())
            .filter(|s| !s.is_empty() && s.len() <= 64)
            .map(str::to_string)
            .collect()
    }
}

// ---------------------------------------------------------------------------
// WhitespaceTokenizer — for code or pre-tokenized inputs.
// ---------------------------------------------------------------------------
pub struct WhitespaceTokenizer;

impl Tokenizer for WhitespaceTokenizer {
    fn tokenize(&self, text: &str) -> Vec<String> {
        text.split_whitespace().map(|s| s.to_lowercase()).collect()
    }
}
