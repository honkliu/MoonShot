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

#[derive(Debug, Clone, Copy)]
#[allow(non_snake_case)]
pub struct QueryCompileModeParameters {
    pub QMP_UnigramWeight: f32,
    pub QMP_BigramWeight: f32,
    pub QMP_BigramBoostWeight: f32,
    pub QMP_StaticWeight: f32,
    pub QMP_PriorWeight: f32,
    pub QMP_QualityWeight: f32,
    pub QMP_AuthorityWeight: f32,
    pub QMP_SpamPenalty: f32,
    pub QMP_CosineWeight: f32,
    pub QMP_AnchorWeight: f32,
    pub QMP_UrlWeight: f32,
    pub QMP_TitleWeight: f32,
    pub QMP_BodyWeight: f32,
}

pub const kWeakAndBigramParameters: QueryCompileModeParameters = QueryCompileModeParameters {
    QMP_UnigramWeight: 1.8,
    QMP_BigramWeight: 0.2,
    QMP_BigramBoostWeight: 0.0,
    QMP_StaticWeight: 1.0,
    QMP_PriorWeight: 0.0,
    QMP_QualityWeight: 1.0,
    QMP_AuthorityWeight: 0.5,
    QMP_SpamPenalty: 2.0,
    QMP_CosineWeight: 16.0,
    QMP_AnchorWeight: 1.0,
    QMP_UrlWeight: 1.0,
    QMP_TitleWeight: 1.0,
    QMP_BodyWeight: 1.0,
};

pub const kWeakAndBigramBoostParameters: QueryCompileModeParameters = QueryCompileModeParameters {
    QMP_UnigramWeight: 0.25,
    QMP_BigramWeight: 1.0,
    QMP_BigramBoostWeight: 0.5,
    QMP_StaticWeight: 0.25,
    QMP_PriorWeight: 4.0,
    QMP_QualityWeight: 0.25,
    QMP_AuthorityWeight: 0.0,
    QMP_SpamPenalty: 4.0,
    QMP_CosineWeight: 16.0,
    QMP_AnchorWeight: 1.0,
    QMP_UrlWeight: 1.0,
    QMP_TitleWeight: 1.0,
    QMP_BodyWeight: 1.0,
};

pub const kWeakAndBigramBoostForDocParameters: QueryCompileModeParameters = QueryCompileModeParameters {
    QMP_UnigramWeight: 0.09,
    QMP_BigramWeight: 0.5,
    QMP_BigramBoostWeight: 4.0,
    QMP_StaticWeight: 0.25,
    QMP_PriorWeight: 2.0,
    QMP_QualityWeight: 1.0,
    QMP_AuthorityWeight: 0.1,
    QMP_SpamPenalty: 1.0,
    QMP_CosineWeight: 128.0,
    QMP_AnchorWeight: 1.0,
    QMP_UrlWeight: 1.0,
    QMP_TitleWeight: 1.0,
    QMP_BodyWeight: 1.0,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueryCompileMode {
    Default,
    WeakAndBigram,
    WeakAndBigramBoost,
    WeakAndBigramBoostForDoc,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeType { Term, And, Or, Not, WeakAnd, Boost }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WeakAndBuildMode { FlatPruned, OrChildren, OrChildrenPruned }

#[allow(non_snake_case)]
pub fn GetQueryCompileModeParameters(mode: QueryCompileMode) -> &'static QueryCompileModeParameters {
    match mode {
        QueryCompileMode::WeakAndBigramBoost => &kWeakAndBigramBoostParameters,
        QueryCompileMode::WeakAndBigramBoostForDoc => &kWeakAndBigramBoostForDocParameters,
        _ => &kWeakAndBigramParameters,
    }
}

impl Default for QueryCompileModeParameters {
    fn default() -> Self { kWeakAndBigramParameters }
}

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

impl TermNode {
    pub fn new(key: String) -> Self {
        Self { stream_key: key, word_span: 1 }
    }

    pub fn with_span(key: String, span: u32) -> Self {
        Self { stream_key: key, word_span: span }
    }
}

#[derive(Debug, Clone)]
pub struct AndNode {
    pub children: Vec<EvalNode>,
}

impl Default for AndNode {
    fn default() -> Self { Self { children: Vec::new() } }
}

#[derive(Debug, Clone)]
pub struct OrNode {
    pub children: Vec<EvalNode>,
}

impl Default for OrNode {
    fn default() -> Self { Self { children: Vec::new() } }
}

#[derive(Debug, Clone)]
pub struct WeakAndNode {
    pub children: Vec<EvalNode>,
    pub min_should_match: u32,
}

impl Default for WeakAndNode {
    fn default() -> Self { Self { children: Vec::new(), min_should_match: 1 } }
}

#[derive(Debug, Clone)]
pub struct NotNode {
    pub base:    Box<EvalNode>,
    pub exclude: Box<EvalNode>,
}

impl NotNode {
    pub fn new(base: EvalNode, exclude: EvalNode) -> Self {
        Self { base: Box::new(base), exclude: Box::new(exclude) }
    }
}

#[derive(Debug, Clone)]
pub struct BoostNode {
    pub base: Box<EvalNode>,
    pub boost: Box<EvalNode>,
    pub boost_weight: f32,
}

impl BoostNode {
    pub fn new(base: EvalNode, boost: EvalNode) -> Self {
        Self { base: Box::new(base), boost: Box::new(boost), boost_weight: 1.0 }
    }

    pub fn with_weight(base: EvalNode, boost: EvalNode, boost_weight: f32) -> Self {
        Self { base: Box::new(base), boost: Box::new(boost), boost_weight }
    }
}

#[derive(Debug, Clone)]
pub enum EvalNode {
    Term(TermNode),
    And(AndNode),
    Or(OrNode),
    Not(NotNode),
    WeakAnd(WeakAndNode),
    Boost(BoostNode),
}

impl EvalNode {
    #[allow(non_snake_case)]
    pub fn GetType(&self) -> NodeType {
        match self {
            Self::Term(_) => NodeType::Term,
            Self::And(_) => NodeType::And,
            Self::Or(_) => NodeType::Or,
            Self::Not(_) => NodeType::Not,
            Self::WeakAnd(_) => NodeType::WeakAnd,
            Self::Boost(_) => NodeType::Boost,
        }
    }
}

#[derive(Debug, Clone)]
pub struct EvalTree {
    pub root: Option<EvalNode>,
    pub vector_query: Vec<f32>,
    pub vector_ef_search: usize,
}

impl EvalTree {
    pub fn new(root: Option<EvalNode>) -> Self { Self { root, vector_query: Vec::new(), vector_ef_search: 200 } }
    pub fn empty()                     -> Self { Self::new(None) }
    #[allow(non_snake_case)]
    pub fn HasTextQuery(&self)         -> bool { self.root.is_some() }
    #[allow(non_snake_case)]
    pub fn HasVectorQuery(&self)       -> bool { !self.vector_query.is_empty() }
    #[allow(non_snake_case)]
    pub fn IsEmpty(&self)              -> bool { !self.HasTextQuery() && !self.HasVectorQuery() }
    pub fn is_empty(&self)             -> bool { self.IsEmpty() }
}

impl Default for EvalTree {
    fn default() -> Self { Self::empty() }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct EvalItem;
