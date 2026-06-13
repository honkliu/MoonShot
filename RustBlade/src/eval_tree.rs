/*
* EvalTree — compiled query AST.
*
* The compiler tokenizes the query string and produces a tree of:
*   TermNode — single (term + stream) posting list key, e.g. "raceT"
*   AndNode  — DAAT intersection of children
*   OrNode   — DAAT union of children
*   NotNode  — base minus exclude
*
* OR nodes are also generated internally to cover multiple streams
* (A/U/T/B) per term and to combine bigram/unigram arms.
* Users never write OR explicitly — all query operators are implicit.
*
* Mirrors MoonShot's EvalExpression.h.
*/

/*
* Bigram separator — mirrors C++ BIGRAM_SEP / REF's CreateBigramString.
* \x1F (ASCII Unit Separator) is never produced by the word tokenizer,
* so "morning\x1Fcall" (bigram) is unambiguous from "morning_call" (unigram).
*/
pub const BIGRAM_SEP: char = '\x1F';

#[derive(Debug, Clone)]
pub struct TermNode {
    pub stream_key: String,
    /// 1 = unigram (REF AtomType_Unigram), 2 = bigram (REF AtomType_Bigram / wordSpan=2)
    pub word_span: u32,
}

#[derive(Debug, Clone)]
pub struct AndNode {
    pub children: Vec<EvalNode>,
}

#[derive(Debug, Clone)]
pub struct OrNode {
    pub children: Vec<EvalNode>,
}

#[derive(Debug, Clone)]
pub struct NotNode {
    pub base:    Box<EvalNode>,
    pub exclude: Box<EvalNode>,
}

#[derive(Debug, Clone)]
pub enum EvalNode {
    Term(TermNode),
    And(AndNode),
    Or(OrNode),
    Not(NotNode),
}

#[derive(Debug, Clone)]
pub struct EvalTree {
    pub root: Option<EvalNode>,
}

impl EvalTree {
    pub fn new(root: Option<EvalNode>) -> Self { Self { root } }
    pub fn empty()                     -> Self { Self { root: None } }
    pub fn is_empty(&self)             -> bool { self.root.is_none() }
}
