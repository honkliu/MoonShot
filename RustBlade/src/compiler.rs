use crate::eval_tree::{EvalTree, EvalNode, TermNode, AndNode, OrNode, NotNode, BIGRAM_SEP};
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
    tokenizer: Box<dyn Tokenizer>,
}

impl IndexSearchCompiler {
    pub fn new(tokenizer: impl Tokenizer + 'static) -> Self {
        Self { tokenizer: Box::new(tokenizer) }
    }

    #[allow(non_snake_case)]
    pub fn Compile(&self, query: &str, stream_set: &str) -> EvalTree {
        let streams = parse_stream_set(stream_set);
        let root = if streams.is_empty() { None } else { parse_expression(query, &streams, self.tokenizer.as_ref()) };
        EvalTree::new(root)
    }
}

fn parse_stream_set(s: &str) -> Vec<String> {
    let mut streams: Vec<String> = s.chars()
        .filter_map(|c| match c.to_ascii_uppercase() {
            'A' => Some("A".into()),
            'U' => Some("U".into()),
            'T' => Some("T".into()),
            'B' => Some("B".into()),
            'M' => Some("M".into()),
            _   => None,
        })
        .collect();
    if streams.is_empty() { streams.push("T".into()); }
    streams
}

fn make_term_group(term: &str, streams: &[String], word_span: u32) -> EvalNode {
    if streams.len() == 1 {
        return EvalNode::Term(TermNode {
            stream_key: format!("{}{}", term, streams[0]),
            word_span,
        });
    }
    EvalNode::Or(OrNode {
        children: streams.iter()
            .map(|s| EvalNode::Term(TermNode {
                stream_key: format!("{}{}", term, s),
                word_span,
            }))
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
        "type" | "filetype" => vec!["U".into()],
        _ => fallback.to_vec(),
    }
}

fn is_or_token(raw: &str) -> bool { raw.eq_ignore_ascii_case("or") }
fn is_not_token(raw: &str) -> bool { raw == "-" || raw.eq_ignore_ascii_case("not") }

fn split_raw_items(query: &str) -> Vec<String> {
    let mut items = Vec::new();
    let mut current = String::new();
    for ch in query.chars() {
        if ch.is_whitespace() || ch == ',' || ch == '(' || ch == ')' {
            if !current.is_empty() {
                items.push(std::mem::take(&mut current));
            }
        } else {
            current.push(ch);
        }
    }
    if !current.is_empty() { items.push(current); }
    items
}

#[derive(Clone)]
struct QueryTerm {
    term: String,
    streams: Vec<String>,
}

fn add_raw_item(raw: &str,
                default_streams: &[String],
                tokenizer: &dyn Tokenizer,
                positive: &mut Vec<QueryTerm>,
                negative: &mut Vec<QueryTerm>,
                force_exclude: bool) {
    if raw.is_empty() || is_or_token(raw) || is_not_token(raw) { return; }
    let has_minus_prefix = raw.starts_with('-') && raw.len() > 1;
    let exclude = has_minus_prefix || force_exclude;
    let mut item = if has_minus_prefix { raw[1..].to_string() } else { raw.to_string() };
    if item.is_empty() { return; }
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
        target.push(QueryTerm { term: token, streams: streams.clone() });
    }
}

fn make_query_term_group(term: &QueryTerm, word_span: u32) -> EvalNode {
    make_term_group(&term.term, &term.streams, word_span)
}

fn build_bigram_query(terms: &[QueryTerm]) -> Option<EvalNode> {
    if terms.len() < 2 { return None; }
    let groups: Vec<EvalNode> = terms.windows(2)
        .filter(|w| !w[0].term.is_empty() && !w[1].term.is_empty() && w[0].streams == w[1].streams)
        .map(|w| make_term_group(
            &format!("{}{}{}", w[0].term, BIGRAM_SEP, w[1].term),
            &w[0].streams,
            2,  /* word_span = 2, mirrors REF AtomType_Bigram */
        ))
        .collect();
    if groups.is_empty()   { return None; }
    if groups.len() == 1   { return Some(groups.into_iter().next().unwrap()); }
    Some(EvalNode::And(AndNode { children: groups }))
}

fn build_query(tokens: &[QueryTerm]) -> Option<EvalNode> {
    let free_nodes: Vec<EvalNode> = tokens.iter()
        .map(|t| make_query_term_group(t, 1))
        .collect();

    if free_nodes.is_empty() { return None; }

    let unigram_base = if free_nodes.len() == 1 {
        free_nodes.into_iter().next().unwrap()
    } else {
        EvalNode::And(AndNode { children: free_nodes })
    };

    match build_bigram_query(tokens) {
        Some(bigram) => Some(EvalNode::Or(OrNode {
            children: vec![bigram, unigram_base],
        })),
        None => Some(unigram_base),
    }
}

fn build_minus_expression(positive: &[QueryTerm], negative: &[QueryTerm]) -> Option<EvalNode> {
    if negative.is_empty() { return build_query(positive); }
    if positive.is_empty() { return None; }
    Some(EvalNode::Not(NotNode {
        base: Box::new(build_query(positive)?),
        exclude: Box::new(build_query(negative)?),
    }))
}

fn parse_expression(query: &str, streams: &[String], tokenizer: &dyn Tokenizer) -> Option<EvalNode> {
    let mut disjuncts = Vec::new();
    let mut positive = Vec::new();
    let mut negative = Vec::new();
    let mut saw_any = false;
    let mut next_is_negative = false;

    for raw in split_raw_items(query) {
        if is_or_token(&raw) {
            if let Some(node) = build_minus_expression(&positive, &negative) {
                disjuncts.push(node);
            }
            positive.clear();
            negative.clear();
            next_is_negative = false;
        } else if is_not_token(&raw) {
            next_is_negative = true;
        } else {
            add_raw_item(&raw, streams, tokenizer, &mut positive, &mut negative, next_is_negative);
            next_is_negative = false;
            saw_any = true;
        }
    }

    if !saw_any { return None; }
    if let Some(node) = build_minus_expression(&positive, &negative) {
        disjuncts.push(node);
    }
    if disjuncts.is_empty() { return None; }
    if disjuncts.len() == 1 { return disjuncts.into_iter().next(); }
    Some(EvalNode::Or(OrNode { children: disjuncts }))
}
