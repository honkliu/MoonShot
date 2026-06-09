# MoonShot Query Syntax Specification

> **Status**: Design document — drives compiler and executor implementation.  
> **Scope**: Covers all query constructs the index must support, ordered from the
> simplest user-facing expression to the internal representation the compiler
> produces.

---

## 1. Core Concepts

### 1.1 MetaStream

A **MetaStream** is a named slice of a document.  Every term that appears in a
document is indexed separately for each MetaStream in which it occurs.

| ID | Name   | Abbreviation | Description                                      |
|----|--------|--------------|--------------------------------------------------|
| 1  | Anchor | A            | Anchor text of in-bound hyperlinks               |
| 2  | URL    | U            | Tokens extracted from the URL / file path        |
| 3  | Title  | T            | Document title or `<h1>` heading                 |
| 4  | Body   | B            | Main prose content                               |
| 5  | Meta   | M            | HTML `<meta>` description, keywords              |
| 6+ | Custom | (user-defined) | Any additional named stream (e.g. `Code`, `Alt`) |

**Posting key convention**: `<term><StreamAbbrev>`  
Examples: `badT` (the word "bad" in Title), `badB` (in Body), `badA` (in Anchor).

> MetaStream IDs 1–5 are reserved.  Custom streams start at 6.

---

### 1.2 Metaword

A **Metaword** is a structured constraint that is _not_ a plain text term.
Metawords are expressed with a colon prefix in the raw query.

| Metaword syntax       | Meaning                                     |
|-----------------------|---------------------------------------------|
| `site:example.com`    | URL must be under `example.com`             |
| `url:example.com`     | Same as `site:` (alias)                     |
| `title:hello`         | "hello" must appear in the Title stream     |
| `anchor:hello`        | "hello" must appear in the Anchor stream    |
| `body:hello`          | "hello" must appear in the Body stream      |
| `lang:en`             | Document language filter                    |
| `type:pdf`            | Document type / MIME filter                 |
| `filetype:pdf`        | Alias for `type:`                           |
| `norank:hello`        | Index "hello" but do NOT use it for ranking |
| `-hello`              | Exclude documents containing "hello" (NOT)  |

---

### 1.3 DocID Ordering

Documents are assigned DocIDs at index-build time, sorted **ascending by
importance** (PageRank score or equivalent).  Lower DocID → more important
document.  This ordering is critical: posting-list traversal naturally visits
the most important documents first, enabling early-exit optimisations.

---

## 2. Query Surface Syntax

### 2.1 Basic Term

A bare word matches documents that contain it in **any** stream.

```
rust
```

Internally compiled to:

```
Or( TermNode("rustA"), TermNode("rustU"),
    TermNode("rustT"), TermNode("rustB") )
```

The set of streams searched by default is the **AUT** group
(Anchor + URL + Title).  Body is added in a later phase (see §4).

---

### 2.2 Phrase

Quoted text requires terms to appear **adjacent** (within a configurable
positional window).

```
"good morning vietnam"
```

Compiled to a `PhraseNode` containing the ordered terms.  Position offsets
are stored per posting entry to evaluate adjacency at query time.

---

### 2.3 Boolean Operators

| Operator   | Symbol / keyword | Semantics               |
|------------|------------------|-------------------------|
| Conjunction | (implicit space) | AND — all terms required |
| Disjunction | `OR`             | OR — at least one term  |
| Negation    | `NOT` or `-`     | Exclude matching docs   |
| Grouping    | `( )`            | Override precedence     |

```
rust safety             → AND(rust, safety)
rust OR go              → OR(rust, go)
rust NOT unsafe         → NOT(base=rust, exclude=unsafe)
-unsafe rust            → same as above
(rust OR go) safety     → AND( OR(rust,go), safety )
```

**Precedence** (high → low): phrase > NOT > AND > OR

---

### 2.4 Field / Stream Constraints

A term can be pinned to a specific MetaStream:

```
title:rust          → TermNode("rustT")   only Title stream
anchor:rust         → TermNode("rustA")   only Anchor stream
body:rust           → TermNode("rustB")   only Body stream
```

Multiple fields OR'd together:

```
title:rust body:rust
```

is compiled as:

```
AND( OR(rustT, rustB), OR(rustT, rustB) )   →  simplified to  OR(rustT, rustB)
```

---

### 2.5 Range Constraint

Applicable to numeric or date doc-data fields.

```
year:2000..2010     → RangeNode(field="year", lo=2000, hi=2010)
size:>1024          → RangeNode(field="size", lo=1024, hi=∞)
```

---

### 2.6 Vector / Embedding Query

Requests approximate nearest-neighbour search on a named vector field.

```
knn(embedding, [0.12, 0.34, ...], k=10)
```

Can be combined with text via hybrid syntax (see §2.7).

---

### 2.7 Hybrid Query

Fuses a text sub-query with a vector sub-query using Reciprocal Rank Fusion.

```
hybrid(
  text   = "rust safety",
  vector = knn(embedding, [...], k=20),
  rrf_k  = 60
)
```

---

## 3. Compiled Representation — EvalTree

The compiler converts surface syntax into an **EvalTree** (a.k.a. MatchingTree).
Every node is one of:

| Node type     | Children           | Description                           |
|---------------|--------------------|---------------------------------------|
| `TermNode`    | none               | Single posting-list lookup (term+stream) |
| `PhraseNode`  | ordered TermNodes  | Positional adjacency check            |
| `AndNode`     | ≥ 2 nodes          | All children must match               |
| `OrNode`      | ≥ 2 nodes          | At least one child must match         |
| `NotNode`     | base + exclude     | Exclude docs matching `exclude`       |
| `RangeNode`   | none               | Numeric/date range filter on DocData  |
| `KnnNode`     | none               | Vector ANN search                     |
| `HybridNode`  | text + knn         | RRF fusion of text and vector results |

### EvalTree Example

Surface: `rust safety NOT unsafe`

```
AndNode
├── OrNode                ← "rust" in AUT streams
│   ├── TermNode("rustA")
│   ├── TermNode("rustU")
│   └── TermNode("rustT")
├── OrNode                ← "safety" in AUT streams
│   ├── TermNode("safetyA")
│   ├── TermNode("safetyU")
│   └── TermNode("safetyT")
└── NotNode(exclude)
    └── OrNode            ← "unsafe" in any stream
        ├── TermNode("unsafeA")
        ├── TermNode("unsafeU")
        └── TermNode("unsafeT")
```

---

## 4. Multi-Phase Match Plan

Inspired by REF's `MatchPlan` / `Rewrite` architecture.

Executing the full query at once is expensive.  The compiler emits a
**MatchPlan**: an ordered list of phases, each progressively relaxing the
constraint or expanding the stream set.  Later phases only run if earlier
phases did not collect enough results.

### Standard Two-Phase Plan

| Phase | Streams searched | Goal                             |
|-------|-----------------|----------------------------------|
| 1     | AUT (Anchor, URL, Title) | High-precision, fast   |
| 2     | AUTB (+ Body)           | Higher recall, slower  |

### Phase Trigger Conditions (from REF)

A phase exits early (moves to the next phase) when **any** condition is met:

| Condition                    | Default threshold |
|------------------------------|-------------------|
| `MatchCount ≥ quota`         | 1 000 results     |
| `SeekCount ≥ threshold`      | 500 000 block seeks |
| `IndexPositionRatio ≥ ratio` | 0.5 (50 % of index traversed) |

### Example Plan (3 phases)

```
Phase 1  streams=AUT   quota=200   — strict, high-quality signal
Phase 2  streams=AUTB  quota=1000  — add Body, collect more
Phase 3  streams=AUTB  relax=1     — allow 1 missing term (OR fallback)
```

---

## 5. Scoring — BM25 Per Stream

Each stream has its own **weight** that scales its BM25 contribution.

| Stream | Default weight |
|--------|---------------|
| A (Anchor)  | 4.0  |
| U (URL)     | 2.0  |
| T (Title)   | 3.0  |
| B (Body)    | 1.0  |

Final score for document `d` given query terms `t₁…tₙ`:

```
Score(d) = DocImportance(d)
         + Σᵢ  weight(stream) × BM25(tᵢ, d, stream)
```

`DocImportance(d)` is loaded from the fixed DocData record (see Index Design §3).

---

## 6. Future Extensions

| Feature          | Notes                                                   |
|------------------|---------------------------------------------------------|
| Bigrams / N-grams | Index adjacent pairs to improve phrase precision       |
| Spelling correction | Map "vienam" → "vietnam" before posting lookup       |
| Synonyms         | Expand "fast" → OR(fast, quick, rapid) at compile time  |
| DNN term weights | Replace fixed stream weights with a learned ranker     |
| Semantic re-rank  | After BM25 candidate set, re-rank with vector cosine   |
