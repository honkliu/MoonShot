use crate::tokenizer::Tokenizer;

// ---------------------------------------------------------------------------
// QueryNode — the EvalTree of MoonShot / MatchingTree of REF, in Rust.
//
// Supports the Boolean algebra every real search engine needs, plus KNN and
// Hybrid nodes for the vector side.  The executor in executor.rs maps each
// variant to the correct ISR or vector search path.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone)]
pub enum QueryNode {
    /// Match documents that contain `term`.
    Term(String),

    /// All children must match (DAAT intersection → AndIsr).
    And(Vec<QueryNode>),

    /// At least one child must match (DAAT union → OrIsr).
    Or(Vec<QueryNode>),

    /// `base` must match and `exclude` must NOT (→ NotIsr).
    Not { base: Box<QueryNode>, exclude: Box<QueryNode> },

    /// Approximate nearest-neighbour search on the named vector field.
    Knn { field: String, vector: Vec<f32>, k: usize },

    /// Hybrid: text + vector fused via RRF.
    Hybrid {
        text:    Box<QueryNode>,
        vector:  Vec<f32>,
        field:   String,
        k:       usize,
        /// RRF constant k (default 60).
        rrf_k:   f32,
    },
}

impl QueryNode {
    /// Convenience: wrap a list of Terms in an And node, or return the single
    /// Term directly when only one term is present.
    pub fn and_of_terms(terms: Vec<String>) -> Self {
        match terms.len() {
            0 => QueryNode::And(vec![]),
            1 => QueryNode::Term(terms.into_iter().next().unwrap()),
            _ => QueryNode::And(terms.into_iter().map(QueryNode::Term).collect()),
        }
    }
}

// ---------------------------------------------------------------------------
// QueryParser — turns a query string into a QueryNode.
//
// Supported micro-syntax (inspired by InfinityDB and Lucene):
//   "hello world"         → AND(hello, world)          (default)
//   "hello OR world"      → OR(hello, world)
//   "rust NOT unsafe"     → NOT(rust, unsafe)
//   "rust AND safe"       → AND(rust, safe)             (explicit AND)
// ---------------------------------------------------------------------------
pub struct QueryParser<'a> {
    tokenizer: &'a dyn Tokenizer,
}

impl<'a> QueryParser<'a> {
    pub fn new(tokenizer: &'a dyn Tokenizer) -> Self {
        Self { tokenizer }
    }

    pub fn parse(&self, query: &str) -> QueryNode {
        let query = query.trim();
        if query.is_empty() {
            return QueryNode::And(vec![]);
        }

        // Detect top-level OR / NOT operators (case-sensitive keywords).
        if let Some(parts) = split_on_keyword(query, " OR ") {
            let children = parts.into_iter()
                .map(|p| self.parse(p))
                .collect();
            return QueryNode::Or(children);
        }

        if let Some((base, excl)) = split_on_keyword(query, " NOT ").and_then(|p| {
            let mut it = p.into_iter();
            Some((it.next()?, it.next()?))
        }) {
            return QueryNode::Not {
                base:    Box::new(self.parse(base)),
                exclude: Box::new(self.parse(excl)),
            };
        }

        // Strip explicit AND keyword and treat remaining tokens as AND.
        let cleaned: String = query.replace(" AND ", " ");
        let tokens = self.tokenizer.tokenize(&cleaned);
        QueryNode::and_of_terms(tokens)
    }
}

// Split `s` on the first occurrence of `keyword`; returns None if not found.
fn split_on_keyword<'a>(s: &'a str, keyword: &str) -> Option<Vec<&'a str>> {
    if !s.contains(keyword) {
        return None;
    }
    Some(s.split(keyword).collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tokenizer::SimpleTokenizer;

    fn parser() -> QueryParser<'static> {
        // leak a simple tokenizer for test purposes
        let tok: &'static SimpleTokenizer = Box::leak(Box::new(SimpleTokenizer));
        QueryParser::new(tok)
    }

    #[test]
    fn parse_single_term() {
        let p = parser();
        let q = p.parse("rust");
        assert!(matches!(q, QueryNode::Term(t) if t == "rust"));
    }

    #[test]
    fn parse_and_default() {
        let p = parser();
        let q = p.parse("hello world");
        assert!(matches!(q, QueryNode::And(_)));
    }

    #[test]
    fn parse_or() {
        let p = parser();
        let q = p.parse("cat OR dog");
        assert!(matches!(q, QueryNode::Or(_)));
    }

    #[test]
    fn parse_not() {
        let p = parser();
        let q = p.parse("rust NOT unsafe");
        assert!(matches!(q, QueryNode::Not { .. }));
    }
}
