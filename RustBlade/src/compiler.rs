use crate::eval_tree::{EvalTree, EvalNode, TermNode, AndNode, OrNode};
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

    pub fn compile(&self, query: &str, stream_set: &str) -> EvalTree {
        let streams = parse_stream_set(stream_set);
        let tokens  = self.tokenizer.tokenize(query);
        if tokens.is_empty() { return EvalTree::empty(); }

        let root = build_query(&tokens, &streams);
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

fn make_term_group(term: &str, streams: &[String]) -> EvalNode {
    if streams.len() == 1 {
        return EvalNode::Term(TermNode { stream_key: format!("{}{}", term, streams[0]) });
    }
    EvalNode::Or(OrNode {
        children: streams.iter()
            .map(|s| EvalNode::Term(TermNode { stream_key: format!("{}{}", term, s) }))
            .collect(),
    })
}

fn build_bigram_query(terms: &[String], streams: &[String]) -> Option<EvalNode> {
    if terms.len() < 2 { return None; }
    let groups: Vec<EvalNode> = terms.windows(2)
        .filter(|w| !w[0].is_empty() && !w[1].is_empty())
        .map(|w| make_term_group(&format!("{}_{}", w[0], w[1]), streams))
        .collect();
    if groups.is_empty()   { return None; }
    if groups.len() == 1   { return Some(groups.into_iter().next().unwrap()); }
    Some(EvalNode::And(AndNode { children: groups }))
}

fn build_query(tokens: &[String], streams: &[String]) -> Option<EvalNode> {
    let free_nodes: Vec<EvalNode> = tokens.iter()
        .map(|t| make_term_group(t, streams))
        .collect();

    let unigram_base = if free_nodes.len() == 1 {
        free_nodes.into_iter().next().unwrap()
    } else {
        EvalNode::And(AndNode { children: free_nodes })
    };

    match build_bigram_query(tokens, streams) {
        Some(bigram) => Some(EvalNode::Or(OrNode {
            children: vec![bigram, unigram_base],
        })),
        None => Some(unigram_base),
    }
}
