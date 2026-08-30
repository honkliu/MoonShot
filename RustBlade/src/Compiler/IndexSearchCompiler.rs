//! Direct translation of the C++ query compiler; symbol names stay aligned for debugging.
#![allow(non_snake_case, non_upper_case_globals)]

use std::collections::HashSet;

use crate::embeddings::{build_hashed_embedding, IEmbeddingModel};
use crate::eval_expression::{
    AndNode, BoostNode, EvalNode, EvalTree, GetQueryCompileModeParameters, NotNode, OrNode,
    QueryCompileMode, TermNode, WeakAndNode, BIGRAM_SEP,
};
use crate::tokenizer::Tokenizer;

/*
* IndexSearchCompiler — tokenizes a query string and builds an EvalTree.
*
* Query and documents are tokenized identically — no special syntax.
* Internal OR nodes cover multiple streams (A/U/T/B) per term.
* Adjacent tokens generate a bigram arm for phrase scoring:
*
*   "race car" → Or( race_carStreams,
*                    And(Or(raceStreams), Or(carStreams)) )
*
* The bigram arm scores higher when tokens appear adjacent.
* Mirrors MoonShot's IndexSearchCompiler.h.
*/
pub struct IndexSearchCompiler {
    m_Tokenizer: Box<dyn Tokenizer>,
}

impl IndexSearchCompiler {
    pub fn new(tokenizer: impl Tokenizer + 'static) -> Self {
        Self {
            m_Tokenizer: Box::new(tokenizer),
        }
    }

    #[allow(non_snake_case)]
    pub fn Compile(&self, query: &str, stream_set: &str) -> EvalTree {
        self.CompileWithMode(query, stream_set, QueryCompileMode::Default)
    }

    #[allow(non_snake_case)]
    pub fn CompileWithMode(
        &self,
        query: &str,
        stream_set: &str,
        mode: QueryCompileMode,
    ) -> EvalTree {
        if query.is_empty() {
            return EvalTree::empty();
        }
        let streams = ParseStreamSet(stream_set);
        let root = if streams.is_empty() {
            None
        } else {
            parse_expression(query, &streams, self.m_Tokenizer.as_ref(), mode)
        };
        EvalTree::new(root)
    }

    #[allow(non_snake_case)]
    pub fn CompileWithEmbeddingModel(
        &self,
        query: &str,
        stream_set: &str,
        embedding_model: Option<&dyn IEmbeddingModel>,
        mode: QueryCompileMode,
    ) -> EvalTree {
        if query.is_empty() {
            return EvalTree::empty();
        }
        let streams = ParseStreamSet(stream_set);
        let root = if streams.is_empty() {
            None
        } else {
            parse_expression(query, &streams, self.m_Tokenizer.as_ref(), mode)
        };
        let mut tree = EvalTree::new(root);
        if HasVectorStream(stream_set) {
            if let Some(model) = embedding_model {
                tree.vector_query = self.CompileToVectorWithModel(query, model);
            }
        }
        tree
    }

    #[allow(non_snake_case)]
    pub fn CompileToVector(&self, query: &str) -> Vec<f32> {
        build_hashed_embedding(&self.m_Tokenizer.Tokenize(query))
    }

    #[allow(non_snake_case)]
    pub fn CompileToVectorWithModel(&self, query: &str, model: &dyn IEmbeddingModel) -> Vec<f32> {
        if query.is_empty() {
            return vec![0.0; model.GetDimension()];
        }
        model.Embed(&self.m_Tokenizer.Tokenize(query))
    }
}

#[allow(non_snake_case)]
fn ParseStreamSet(s: &str) -> Vec<String> {
    let mut saw_vector_only_candidate = false;
    let mut streams: Vec<String> = s
        .chars()
        .filter_map(|c| match c {
            'A' => Some("A".into()),
            'U' => Some("U".into()),
            'T' => Some("T".into()),
            'B' => Some("B".into()),
            'M' => Some("M".into()),
            'V' | 'v' => {
                saw_vector_only_candidate = true;
                None
            }
            _ => None,
        })
        .collect();
    if streams.is_empty() && !saw_vector_only_candidate {
        streams.push("T".into());
    }
    streams
}

#[allow(non_snake_case)]
fn HasVectorStream(s: &str) -> bool {
    s.chars().any(|c| c == 'V' || c == 'v')
}

fn make_term_group(term: &str, streams: &[String], word_span: u32) -> EvalNode {
    if streams.len() == 1 {
        return EvalNode::Term(TermNode {
            stream_key: format!("{}{}", term, streams[0]),
            word_span,
        });
    }
    EvalNode::Or(OrNode {
        children: streams
            .iter()
            .map(|s| {
                EvalNode::Term(TermNode {
                    stream_key: format!("{}{}", term, s),
                    word_span,
                })
            })
            .collect(),
    })
}

fn streams_for_field(field: &str, fallback: &[String]) -> Vec<String> {
    match field.to_ascii_lowercase().as_str() {
        "title" => vec!["T".into()],
        "body" => vec!["B".into()],
        "url" | "site" => vec!["U".into()],
        "anchor" => vec!["A".into()],
        "meta" => vec!["M".into()],
        _ => fallback.to_vec(),
    }
}

fn is_or_token(raw: &str) -> bool {
    raw.eq_ignore_ascii_case("or")
}
fn is_not_token(raw: &str) -> bool {
    matches!(raw, "not" | "NOT" | "Not" | "nOT" | "-")
}

fn split_raw_items(query: &str) -> Vec<String> {
    let mut items = Vec::new();
    let mut current = String::new();
    for ch in query.chars() {
        if ch.is_ascii_whitespace() || ch == ',' || ch == '(' || ch == ')' {
            if !current.is_empty() {
                items.push(std::mem::take(&mut current));
            }
        } else {
            current.push(ch);
        }
    }
    if !current.is_empty() {
        items.push(current);
    }
    items
}

#[derive(Clone)]
struct QueryTerm {
    term: String,
    streams: Vec<String>,
}

fn query_term_key(term: &QueryTerm) -> String {
    let mut key = term.term.clone();
    key.push('\x1e');
    for stream in &term.streams {
        key.push_str(stream);
        key.push(',');
    }
    key
}

fn filter_weak_and_terms(tokens: &[QueryTerm]) -> Vec<QueryTerm> {
    let mut seen = HashSet::new();
    let mut filtered: Vec<QueryTerm> = tokens
        .iter()
        .filter(|token| token.term.len() > 1)
        .filter(|token| seen.insert(query_term_key(token)))
        .cloned()
        .collect();
    if !filtered.is_empty() {
        return filtered;
    }
    filtered.extend(
        tokens
            .iter()
            .filter(|token| !token.term.is_empty())
            .filter(|token| seen.insert(query_term_key(token)))
            .cloned(),
    );
    filtered
}

fn add_raw_item(
    raw: &str,
    default_streams: &[String],
    tokenizer: &dyn Tokenizer,
    positive: &mut Vec<QueryTerm>,
    negative: &mut Vec<QueryTerm>,
    force_exclude: bool,
) {
    if raw.is_empty() || is_or_token(raw) || is_not_token(raw) {
        return;
    }
    let has_minus_prefix = raw.starts_with('-') && raw.len() > 1;
    let exclude = has_minus_prefix || force_exclude;
    let mut item = if has_minus_prefix {
        raw[1..].to_string()
    } else {
        raw.to_string()
    };
    if item.is_empty() {
        return;
    }
    let mut streams = default_streams.to_vec();

    if let Some(colon) = item.find(':') {
        if colon > 0 && colon + 1 < item.len() {
            let field = item[..colon].to_string();
            streams = streams_for_field(&field, default_streams);
            item = item[colon + 1..].to_string();
        }
    }

    let target = if exclude { negative } else { positive };
    for token in tokenizer.Tokenize(&item) {
        target.push(QueryTerm {
            term: token,
            streams: streams.clone(),
        });
    }
}

fn make_query_term_group(term: &QueryTerm, word_span: u32) -> EvalNode {
    make_term_group(&term.term, &term.streams, word_span)
}

fn build_bigram_query(terms: &[QueryTerm]) -> Option<EvalNode> {
    if terms.len() < 2 {
        return None;
    }
    let groups: Vec<EvalNode> = terms
        .windows(2)
        .filter(|w| w[0].streams == w[1].streams)
        .map(|w| {
            make_term_group(
                &format!("{}{}{}", w[0].term, BIGRAM_SEP, w[1].term),
                &w[0].streams,
                2, /* word_span = 2, mirrors REF AtomType_Bigram */
            )
        })
        .collect();
    if groups.is_empty() {
        return None;
    }
    if groups.len() == 1 {
        return Some(groups.into_iter().next().unwrap());
    }
    Some(EvalNode::And(AndNode { children: groups }))
}

fn build_any_bigram_query(terms: &[QueryTerm]) -> Option<EvalNode> {
    if terms.len() < 2 {
        return None;
    }
    let groups: Vec<EvalNode> = terms
        .windows(2)
        .filter(|window| window[0].streams == window[1].streams)
        .map(|window| {
            make_term_group(
                &format!("{}{}{}", window[0].term, BIGRAM_SEP, window[1].term),
                &window[0].streams,
                2,
            )
        })
        .collect();
    match groups.len() {
        0 => None,
        1 => groups.into_iter().next(),
        _ => Some(EvalNode::Or(OrNode { children: groups })),
    }
}

fn min_should_match(term_count: usize) -> u32 {
    if term_count <= 2 {
        1
    } else if term_count <= 5 {
        2
    } else {
        3
    }
}

fn build_weak_and_base_expression(terms: &[QueryTerm]) -> Option<EvalNode> {
    if terms.is_empty() {
        return None;
    }
    if terms.len() == 1 {
        return Some(make_query_term_group(&terms[0], 1));
    }
    let children: Vec<EvalNode> = terms
        .iter()
        .map(|term| make_query_term_group(term, 1))
        .collect();
    let min_should_match = min_should_match(terms.len()).min(children.len() as u32);
    Some(EvalNode::WeakAnd(WeakAndNode {
        children,
        min_should_match,
    }))
}

fn build_weak_and_bigram_expression(tokens: &[QueryTerm]) -> Option<EvalNode> {
    let terms = filter_weak_and_terms(tokens);
    let base = build_weak_and_base_expression(&terms);
    let bigram = build_any_bigram_query(&terms);
    match (base, bigram) {
        (Some(base), Some(bigram)) => Some(EvalNode::Or(OrNode {
            children: vec![base, bigram],
        })),
        (Some(base), None) => Some(base),
        (None, bigram) => bigram,
    }
}

fn build_weak_and_bigram_boost_expression(
    tokens: &[QueryTerm],
    mode: QueryCompileMode,
) -> Option<EvalNode> {
    let terms = filter_weak_and_terms(tokens);
    let base = build_weak_and_base_expression(&terms)?;
    let Some(bigram) = build_any_bigram_query(&terms) else {
        return Some(base);
    };
    Some(EvalNode::Boost(BoostNode {
        base: Box::new(base),
        boost: Box::new(bigram),
        boost_weight: GetQueryCompileModeParameters(mode).QMP_BigramBoostWeight,
    }))
}

fn build_implicit_expression(tokens: &[QueryTerm], mode: QueryCompileMode) -> Option<EvalNode> {
    match mode {
        QueryCompileMode::WeakAndBigram => return build_weak_and_bigram_expression(tokens),
        QueryCompileMode::WeakAndBigramBoost | QueryCompileMode::WeakAndBigramBoostForDoc => {
            return build_weak_and_bigram_boost_expression(tokens, mode);
        }
        QueryCompileMode::Default => {}
    }
    let free_nodes: Vec<EvalNode> = tokens.iter().map(|t| make_query_term_group(t, 1)).collect();

    if free_nodes.is_empty() {
        return None;
    }

    let unigram_base = if free_nodes.len() == 1 {
        free_nodes.into_iter().next().unwrap()
    } else {
        EvalNode::And(AndNode {
            children: free_nodes,
        })
    };

    match build_bigram_query(tokens) {
        Some(bigram) => Some(EvalNode::Or(OrNode {
            children: vec![bigram, unigram_base],
        })),
        None => Some(unigram_base),
    }
}

fn build_minus_expression(
    positive: &[QueryTerm],
    negative: &[QueryTerm],
    mode: QueryCompileMode,
) -> Option<EvalNode> {
    if negative.is_empty() {
        return build_implicit_expression(positive, mode);
    }
    if positive.is_empty() {
        return None;
    }
    Some(EvalNode::Not(NotNode {
        base: Box::new(build_implicit_expression(positive, mode)?),
        exclude: Box::new(build_implicit_expression(negative, mode)?),
    }))
}

fn parse_expression(
    query: &str,
    streams: &[String],
    tokenizer: &dyn Tokenizer,
    mode: QueryCompileMode,
) -> Option<EvalNode> {
    let mut disjuncts = Vec::new();
    let mut positive = Vec::new();
    let mut negative = Vec::new();
    let mut saw_any = false;
    let mut next_is_negative = false;

    for raw in split_raw_items(query) {
        if is_or_token(&raw) {
            if let Some(node) = build_minus_expression(&positive, &negative, mode) {
                disjuncts.push(node);
            }
            positive.clear();
            negative.clear();
            next_is_negative = false;
        } else if is_not_token(&raw) {
            next_is_negative = true;
        } else {
            add_raw_item(
                &raw,
                streams,
                tokenizer,
                &mut positive,
                &mut negative,
                next_is_negative,
            );
            next_is_negative = false;
            saw_any = true;
        }
    }

    if !saw_any {
        return None;
    }
    if let Some(node) = build_minus_expression(&positive, &negative, mode) {
        disjuncts.push(node);
    }
    if disjuncts.is_empty() {
        return None;
    }
    if disjuncts.len() == 1 {
        return disjuncts.into_iter().next();
    }
    Some(EvalNode::Or(OrNode {
        children: disjuncts,
    }))
}
