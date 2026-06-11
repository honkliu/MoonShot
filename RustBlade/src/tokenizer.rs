use unicode_segmentation::UnicodeSegmentation;

pub trait Tokenizer: Send + Sync {
    fn tokenize(&self, text: &str) -> Vec<String>;
}

/*
* SmartTokenizer — Unicode word segmentation via unicode-segmentation crate.
* Equivalent to ICU BreakIterator in the C++ MoonShot.
* CJK ideographs are individual words per UAX#29; bigrams are generated
* by AdvancedIndexWriter at index time.
*/
pub struct SmartTokenizer;

impl SmartTokenizer {
    pub fn new() -> Self { Self }
}

impl Default for SmartTokenizer {
    fn default() -> Self { Self::new() }
}

impl Tokenizer for SmartTokenizer {
    fn tokenize(&self, text: &str) -> Vec<String> {
        text.unicode_words()
            .map(|w| w.to_lowercase())
            .filter(|w| !w.is_empty() && w.len() <= 64)
            .collect()
    }
}
