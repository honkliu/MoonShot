use icu_segmenter::{options::WordBreakInvariantOptions, WordSegmenter};
#[cfg(not(any(windows, target_arch = "wasm32")))]
use unicode_normalization::UnicodeNormalization;

pub use crate::tokenizer_interface::Tokenizer;

/*
* SmartTokenizer — ICU word segmentation.
* Mirrors the C++ path: NFC normalization, English lowercasing, Snowball
* English stemming for ASCII lowercase words, then indexability filtering.
*/
pub struct SmartTokenizer {
    m_Locale: String,
}

impl SmartTokenizer {
    pub fn new() -> Self { Self::WithLocale("en") }

    #[allow(non_snake_case)]
    pub fn WithLocale(locale: &str) -> Self {
        Self { m_Locale: locale.to_string() }
    }

    #[allow(non_snake_case)]
    fn IsIndexableToken(token: &str) -> bool {
        if token.is_empty() || token.len() > 64 { return false; }
        if token.bytes().any(|ch| ch < 0x20 || ch == 0x7f) { return false; }

        !token.chars().any(|ch| {
            let value = ch as u32;
            ch.is_control()
                || is_unicode_format(value)
                || matches!(value,
                    0xE000..=0xF8FF |
                    0xF0000..=0xFFFFD |
                    0x100000..=0x10FFFD)
        })
    }
}

impl Default for SmartTokenizer {
    fn default() -> Self { Self::new() }
}

impl Tokenizer for SmartTokenizer {
    fn Tokenize(&self, text: &str) -> Vec<String> {
        if text.is_empty() { return Vec::new(); }

        let normalized = normalize_nfc(text);
        let segmenter = WordSegmenter::new_auto(WordBreakInvariantOptions::default());
        let mut tokens = Vec::new();
        let mut start = 0usize;

        for (end, word_type) in segmenter.segment_str(&normalized).iter_with_word_type() {
            if end > start && word_type.is_word_like() {
                let word = lowercase_for_locale(&normalized[start..end], &self.m_Locale);
                let word = StemEnglishToken(&word);
                if Self::IsIndexableToken(&word) {
                    tokens.push(word);
                }
            }
            start = end;
        }

        tokens
    }
}

fn is_unicode_format(ch: u32) -> bool {
    matches!(ch,
        0x00AD |
        0x0600..=0x0605 | 0x061C | 0x06DD | 0x070F |
        0x0890..=0x0891 | 0x08E2 | 0x180E |
        0x200B..=0x200F | 0x202A..=0x202E |
        0x2060..=0x2064 | 0x2066..=0x206F |
        0xFEFF | 0xFFF9..=0xFFFB |
        0x110BD | 0x110CD | 0x13430..=0x1343F |
        0x1BCA0..=0x1BCA3 | 0x1D173..=0x1D17A |
        0xE0001 | 0xE0020..=0xE007F)
}

fn lowercase_for_locale(text: &str, locale: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    let turkic = locale.eq_ignore_ascii_case("tr") || locale.eq_ignore_ascii_case("tr-tr")
        || locale.eq_ignore_ascii_case("az") || locale.eq_ignore_ascii_case("az-az");
    let mut lowered = String::new();
    for (index, ch) in chars.iter().copied().enumerate() {
        if turkic && ch == 'I' {
            lowered.push('ı');
        } else if turkic && ch == 'İ' {
            lowered.push('i');
        } else if ch == 'Σ'
            && chars[..index].iter().rev().any(|ch| ch.is_alphabetic())
            && !chars[index + 1..].iter().any(|ch| ch.is_alphabetic()) {
            lowered.push('ς');
        } else {
            lowered.extend(ch.to_lowercase());
        }
    }
    lowered
}

#[cfg(windows)]
fn normalize_nfc(text: &str) -> String {
    #[link(name = "Normaliz")]
    extern "system" {
        fn NormalizeString(
            norm_form: i32,
            source: *const u16,
            source_length: i32,
            destination: *mut u16,
            destination_length: i32,
        ) -> i32;
    }

    const NORMALIZATION_C: i32 = 1;
    let source: Vec<u16> = text.encode_utf16().collect();
    if source.len() > i32::MAX as usize { return text.to_string(); }
    let required = unsafe {
        NormalizeString(NORMALIZATION_C, source.as_ptr(), source.len() as i32, std::ptr::null_mut(), 0)
    };
    if required <= 0 { return text.to_string(); }

    let mut destination = vec![0u16; required as usize];
    let written = unsafe {
        NormalizeString(
            NORMALIZATION_C,
            source.as_ptr(),
            source.len() as i32,
            destination.as_mut_ptr(),
            required,
        )
    };
    if written <= 0 { return text.to_string(); }
    destination.truncate(written as usize);
    String::from_utf16(&destination).unwrap_or_else(|_| text.to_string())
}

#[cfg(all(target_arch = "wasm32", feature = "wasm"))]
fn normalize_nfc(text: &str) -> String {
    String::from(js_sys::JsString::from(text).normalize())
}

#[cfg(not(any(windows, target_arch = "wasm32")))]
fn normalize_nfc(text: &str) -> String {
    text.nfc().collect()
}

#[cfg(all(target_arch = "wasm32", not(feature = "wasm")))]
fn normalize_nfc(text: &str) -> String {
    text.to_string()
}

#[allow(non_snake_case)]
fn StemEnglishToken(token: &str) -> String {
    if token.is_empty() || !token.bytes().all(|ch| ch.is_ascii_lowercase()) {
        return token.to_string();
    }
    porter2_stem(token)
}

fn porter2_stem(token: &str) -> String {
    let exceptional = match token {
        "skis" => Some("ski"), "skies" => Some("sky"),
        "dying" => Some("die"), "lying" => Some("lie"), "tying" => Some("tie"),
        "idly" => Some("idl"), "gently" => Some("gentl"), "ugly" => Some("ugli"),
        "early" => Some("earli"), "only" => Some("onli"), "singly" => Some("singl"),
        "sky" | "news" | "howe" | "atlas" | "cosmos" | "bias" | "andes" => Some(token),
        _ => None,
    };
    if let Some(stem) = exceptional { return stem.to_string(); }

    let mut word = token.to_string();
    let marked_y = mark_consonant_y(word.as_bytes());
    let r1 = region_after_vowel_consonant(word.as_bytes(), &marked_y, 0, special_r1(&word));
    let r2 = region_after_vowel_consonant(word.as_bytes(), &marked_y, r1, 0);

    step_1a(&mut word, &marked_y);
    if matches!(word.as_str(), "inning" | "outing" | "canning" | "herring" | "earring" |
        "proceed" | "exceed" | "succeed") {
        return word;
    }
    step_1b(&mut word, r1, &marked_y);
    step_1c(&mut word, &marked_y);
    step_2(&mut word, r1);
    step_3(&mut word, r1, r2);
    step_4(&mut word, r2);
    step_5(&mut word, r1, r2, &marked_y);
    word
}

fn is_vowel(ch: u8) -> bool { matches!(ch, b'a' | b'e' | b'i' | b'o' | b'u' | b'y') }

fn mark_consonant_y(word: &[u8]) -> Vec<bool> {
    let mut marked = vec![false; word.len()];
    for index in 0..word.len() {
        if word[index] == b'y' && (index == 0 || is_vowel_at(word, index - 1, &marked)) {
            marked[index] = true;
        }
    }
    marked
}

fn is_vowel_at(word: &[u8], index: usize, marked_y: &[bool]) -> bool {
    is_vowel(word[index]) && !(word[index] == b'y' && marked_y.get(index).copied().unwrap_or(false))
}

fn special_r1(word: &str) -> usize {
    if word.starts_with("gener") { 5 }
    else if word.starts_with("commun") { 6 }
    else if word.starts_with("arsen") { 5 }
    else { 0 }
}

fn region_after_vowel_consonant(word: &[u8], marked_y: &[bool], start: usize, minimum: usize) -> usize {
    if minimum > 0 { return minimum.min(word.len()); }
    for index in start.saturating_add(1)..word.len() {
        if is_vowel_at(word, index - 1, marked_y) && !is_vowel_at(word, index, marked_y) {
            return index + 1;
        }
    }
    word.len()
}

fn replace_suffix(word: &mut String, suffix: &str, replacement: &str) {
    let length = word.len() - suffix.len();
    word.truncate(length);
    word.push_str(replacement);
}

fn suffix_in_region(word: &str, suffix: &str, region: usize) -> bool {
    word.ends_with(suffix) && word.len() - suffix.len() >= region
}

fn contains_vowel(word: &[u8], end: usize, marked_y: &[bool]) -> bool {
    (0..end).any(|index| is_vowel_at(word, index, marked_y))
}

fn step_1a(word: &mut String, marked_y: &[bool]) {
    if word.ends_with("sses") { replace_suffix(word, "sses", "ss"); }
    else if word.ends_with("ied") || word.ends_with("ies") {
        let suffix = word[word.len() - 3..].to_string();
        let replacement = if word.len() > 4 { "i" } else { "ie" };
        replace_suffix(word, &suffix, replacement);
    } else if word.ends_with("us") || word.ends_with("ss") {
    } else if word.ends_with('s') && word.len() > 2 &&
        (0..word.len() - 2).any(|index| is_vowel_at(word.as_bytes(), index, marked_y)) {
        word.pop();
    }
}

fn step_1b(word: &mut String, r1: usize, original_marked_y: &[bool]) {
    if suffix_in_region(word, "eedly", r1) { replace_suffix(word, "eedly", "ee"); return; }
    if suffix_in_region(word, "eed", r1) { replace_suffix(word, "eed", "ee"); return; }

    let suffix = ["ingly", "edly", "ing", "ed"].into_iter().find(|suffix| word.ends_with(suffix));
    let Some(suffix) = suffix else { return; };
    let stem_length = word.len() - suffix.len();
    if !contains_vowel(word.as_bytes(), stem_length, original_marked_y) { return; }
    word.truncate(stem_length);

    if word.ends_with("at") || word.ends_with("bl") || word.ends_with("iz") {
        word.push('e');
    } else if ["bb", "dd", "ff", "gg", "mm", "nn", "pp", "rr", "tt"]
        .into_iter().any(|ending| word.ends_with(ending)) {
        word.pop();
    } else {
        let marked_y = mark_consonant_y(word.as_bytes());
        if r1 >= word.len() && is_short_syllable(word.as_bytes(), &marked_y) {
            word.push('e');
        }
    }
}

fn is_short_syllable(word: &[u8], marked_y: &[bool]) -> bool {
    if word.len() == 2 {
        return is_vowel_at(word, 0, marked_y) && !is_vowel_at(word, 1, marked_y);
    }
    if word.len() < 3 { return false; }
    let end = word.len() - 1;
    !is_vowel_at(word, end - 2, marked_y)
        && is_vowel_at(word, end - 1, marked_y)
        && !is_vowel_at(word, end, marked_y)
        && !matches!(word[end], b'w' | b'x' | b'y')
}

fn step_1c(word: &mut String, marked_y: &[bool]) {
    if word.len() <= 2 { return; }
    let bytes = word.as_bytes();
    let last = bytes[word.len() - 1];
    if matches!(last, b'y' | b'Y') && !is_vowel_at(bytes, word.len() - 2, marked_y) {
        word.pop();
        word.push('i');
    }
}

fn step_2(word: &mut String, r1: usize) {
    const RULES: [(&str, &str); 23] = [
        ("ization", "ize"), ("ational", "ate"), ("fulness", "ful"), ("ousness", "ous"),
        ("iveness", "ive"), ("tional", "tion"), ("biliti", "ble"), ("lessli", "less"),
        ("entli", "ent"), ("ation", "ate"), ("alism", "al"), ("aliti", "al"),
        ("ousli", "ous"), ("iviti", "ive"), ("fulli", "ful"), ("enci", "ence"),
        ("anci", "ance"), ("abli", "able"), ("izer", "ize"), ("ator", "ate"),
        ("alli", "al"), ("bli", "ble"), ("ogi", "og"),
    ];
    for (suffix, replacement) in RULES {
        if suffix_in_region(word, suffix, r1) {
            if suffix != "ogi" || word.as_bytes().get(word.len() - suffix.len() - 1) == Some(&b'l') {
                replace_suffix(word, suffix, replacement);
            }
            return;
        }
    }
    if suffix_in_region(word, "li", r1) {
        let preceding = word.as_bytes()[word.len() - 3];
        if matches!(preceding, b'c' | b'd' | b'e' | b'g' | b'h' | b'k' | b'm' | b'n' | b'r' | b't') {
            replace_suffix(word, "li", "");
        }
    }
}

fn step_3(word: &mut String, r1: usize, r2: usize) {
    const RULES: [(&str, &str); 8] = [
        ("ational", "ate"), ("tional", "tion"), ("alize", "al"), ("icate", "ic"),
        ("iciti", "ic"), ("ical", "ic"), ("ful", ""), ("ness", ""),
    ];
    for (suffix, replacement) in RULES {
        if suffix_in_region(word, suffix, r1) {
            replace_suffix(word, suffix, replacement);
            return;
        }
    }
    if suffix_in_region(word, "ative", r2) { replace_suffix(word, "ative", ""); }
}

fn step_4(word: &mut String, r2: usize) {
    const SUFFIXES: [&str; 17] = [
        "ement", "ance", "ence", "able", "ible", "ment", "ant", "ent", "ism",
        "ate", "iti", "ous", "ive", "ize", "al", "er", "ic",
    ];
    for suffix in SUFFIXES {
        if suffix_in_region(word, suffix, r2) {
            replace_suffix(word, suffix, "");
            return;
        }
    }
    if suffix_in_region(word, "ion", r2) {
        let preceding = word.as_bytes()[word.len() - 4];
        if matches!(preceding, b's' | b't') { replace_suffix(word, "ion", ""); }
    } else if suffix_in_region(word, "ou", r2) {
        replace_suffix(word, "ou", "");
    }
}

fn step_5(word: &mut String, r1: usize, r2: usize, original_marked_y: &[bool]) {
    if word.ends_with('e') {
        let e_position = word.len() - 1;
        if e_position >= r2 {
            word.pop();
        } else if e_position >= r1 {
            let stem = &word.as_bytes()[..e_position];
            let marked = &original_marked_y[..original_marked_y.len().min(stem.len())];
            if !is_short_syllable(stem, marked) { word.pop(); }
        }
    } else if word.ends_with("ll") && word.len() - 1 >= r2 {
        word.pop();
    }
}

#[cfg(test)]
mod tests {
    use super::{SmartTokenizer, Tokenizer};

    #[test]
    fn keeps_cjk_word_segments() {
        let tokenizer = SmartTokenizer::new();
        assert_eq!(tokenizer.Tokenize("学习"), vec!["学习"]);
    }

    #[test]
    fn stems_english_with_snowball_rules() {
        let tokenizer = SmartTokenizer::new();
        assert_eq!(tokenizer.Tokenize("suggested suggests suggesting"), vec!["suggest", "suggest", "suggest"]);
    }
}
