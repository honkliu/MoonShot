/// Search "morning vietnam" in a movie corpus.
///
/// Walks through what happens internally at each step:
///   Tokenize → QueryNode tree → ISR open → BM25 score → rank
///
/// Run with:   cargo run --example morning_vietnam

use rustblade::{Document, Engine, EngineConfig, Metric};

fn main() -> Result<(), Box<dyn std::error::Error>> {

    // -----------------------------------------------------------------------
    // Corpus — a small set of movie documents.
    // Each doc has title, description, and a 4-dim "genre" embedding:
    //   dim-0: war,  dim-1: comedy,  dim-2: drama,  dim-3: action
    // -----------------------------------------------------------------------
    let movies = vec![
        (1,  "Good Morning, Vietnam",
              "Robin Williams plays a radio DJ stationed in Saigon during the Vietnam War. \
               The film balances morning broadcasts and comedy with the brutal reality of war.",
              [0.80f32, 0.70, 0.60, 0.30]),

        (2,  "Apocalypse Now",
              "A US Army captain journeys through Vietnam and Cambodia in search of a rogue colonel. \
               A harrowing portrait of war and madness.",
              [0.95, 0.05, 0.80, 0.50]),

        (3,  "Platoon",
              "A young soldier survives the chaos and moral ambiguity of the Vietnam War \
               as two rival sergeants battle for his soul.",
              [0.90, 0.10, 0.85, 0.60]),

        (4,  "Full Metal Jacket",
              "A two-part story: brutal Marine boot camp training followed by the Tet \
               Offensive in Vietnam. Kubrick's unflinching view of war.",
              [0.90, 0.15, 0.75, 0.65]),

        (5,  "Forrest Gump",
              "A slow-witted but kind-hearted man from Alabama witnesses historic events \
               including the Vietnam War, the civil rights movement, and Watergate.",
              [0.50, 0.60, 0.90, 0.20]),

        (6,  "The Deer Hunter",
              "Three steelworkers from Pennsylvania go to Vietnam and experience the \
               trauma of captivity and Russian roulette. An epic portrait of loss.",
              [0.85, 0.05, 0.95, 0.20]),

        (7,  "Good Will Hunting",
              "A janitor at MIT hides his extraordinary mathematical genius until a \
               therapist helps him confront his troubled past.",
              [0.05, 0.30, 0.90, 0.10]),

        (8,  "Good Morning, London",
              "A fictional British comedy set in the 1960s about a morning radio station \
               struggling to survive against a backdrop of Swinging London.",
              [0.10, 0.80, 0.40, 0.10]),

        (9,  "Born on the Fourth of July",
              "Ron Kovic, paralyzed in the Vietnam War, transforms from patriotic soldier \
               to impassioned anti-war activist.",
              [0.85, 0.05, 0.90, 0.15]),

        (10, "Hamburger Hill",
              "US soldiers repeatedly assault a heavily fortified North Vietnamese position \
               in one of the bloodiest battles of the Vietnam War.",
              [0.95, 0.05, 0.60, 0.70]),
    ];

    // -----------------------------------------------------------------------
    // Build engine + index
    // -----------------------------------------------------------------------
    let config = EngineConfig::default()
        .with_hnsw_field("embedding", 4, Metric::Cosine);

    let mut engine = Engine::with_config(config);

    for (id, title, body, emb) in &movies {
        engine.add_document(
            Document::new(*id as u64)
                .with_text("title", *title)
                .with_text("body",  *body)
                .with_vector("embedding", emb.to_vec()),
        )?;
    }

    engine.build()?;
    println!("Corpus ready — {} movies indexed.\n", engine.doc_count());

    // -----------------------------------------------------------------------
    // Search 1: exact query
    //
    // Internal path:
    //   "morning vietnam"
    //   → SimpleTokenizer  → ["morning", "vietnam"]
    //   → QueryParser      → And(Term("morning"), Term("vietnam"))
    //   → build_isr        → AndIsr([InvertedIsr("morning"), InvertedIsr("vietnam")])
    //   → DAAT align loop  → finds doc 1 ("Good Morning, Vietnam")
    //   → BM25 score       → IDF("morning") * TF_norm + IDF("vietnam") * TF_norm
    // -----------------------------------------------------------------------
    println!("=== 1. Exact query: \"morning vietnam\" ===");
    let results = engine.search("morning vietnam", 5)?;
    print_results(&results, &movies);

    // -----------------------------------------------------------------------
    // Search 2: the typo you wrote — "morning vienam"
    //
    // "vienam" is NOT in the dictionary, so its ISR returns None.
    // build_isr filters out the None → AndIsr has only one child:
    //   And([InvertedIsr("morning")])  →  collapses to InvertedIsr("morning")
    //
    // Result: all docs that contain "morning", ranked by BM25.
    // Docs 1 and 8 both have "morning" in their title.
    // -----------------------------------------------------------------------
    println!("\n=== 2. Typo query: \"morning vienam\" ===");
    println!("    (\"vienam\" not in dictionary → degrades to single-term \"morning\")");
    let results = engine.search("morning vienam", 5)?;
    print_results(&results, &movies);

    // -----------------------------------------------------------------------
    // Search 3: OR — recall over precision
    //
    //   "morning OR vietnam"
    //   → OrIsr([InvertedIsr("morning"), InvertedIsr("vietnam")])
    //   → DAAT union → returns every doc containing either term
    //   → scores sum contributions from matched terms
    // -----------------------------------------------------------------------
    println!("\n=== 3. OR query: \"morning OR vietnam\" ===");
    let results = engine.search("morning OR vietnam", 5)?;
    print_results(&results, &movies);

    // -----------------------------------------------------------------------
    // Search 4: NOT — exclude comedies
    //
    //   "morning NOT comedy"
    //   → NotIsr(base=InvertedIsr("morning"), exclude=InvertedIsr("comedy"))
    //   → returns docs with "morning" that do NOT contain "comedy"
    // -----------------------------------------------------------------------
    println!("\n=== 4. NOT query: \"morning NOT comedy\" ===");
    let results = engine.search("morning NOT comedy", 5)?;
    print_results(&results, &movies);

    // -----------------------------------------------------------------------
    // Search 5: vector KNN — semantic neighbours of "Good Morning, Vietnam"
    //
    // Query embedding ≈ [0.80, 0.70, 0.60, 0.30] (high war + comedy + drama)
    // HNSW greedy descends from top layer, beam-searches layer 0 with ef=32,
    // returns cosine similarities in descending order.
    // -----------------------------------------------------------------------
    let gmv_embedding = vec![0.80f32, 0.70, 0.60, 0.30];
    println!("\n=== 5. KNN vector search — movies like \"Good Morning, Vietnam\" ===");
    let results = engine.search_knn("embedding", &gmv_embedding, 5)?;
    print_results(&results, &movies);

    // -----------------------------------------------------------------------
    // Search 6: hybrid — best of both worlds
    //
    // text path  → BM25 ranking for "morning vietnam"
    // vector path → HNSW cosine ranking for gmv_embedding
    // RRF fusion: score = Σ  1 / (60 + rank_r)   for each ranking r
    // Docs that rank highly in BOTH lists bubble to the top.
    // -----------------------------------------------------------------------
    println!("\n=== 6. Hybrid: text=\"morning vietnam\" + vector embedding ===");
    println!("    (RRF k=60 fuses BM25 ranks + cosine ranks)");
    let results = engine.search_hybrid("morning vietnam", "embedding", &gmv_embedding, 5)?;
    print_results(&results, &movies);

    Ok(())
}

// -----------------------------------------------------------------------
// Helper
// -----------------------------------------------------------------------
fn print_results(results: &[rustblade::SearchResult], movies: &[(i32, &str, &str, [f32; 4])]) {
    if results.is_empty() {
        println!("  (no results)");
        return;
    }
    for r in results {
        // Look up the original title from our local slice (avoids needing
        // the doc store for this demo).
        let title = movies.iter()
            .find(|(id, ..)| *id as u64 == r.doc_id)
            .map(|(_, t, ..)| *t)
            .unwrap_or("—");
        println!("  [doc {:>2}]  score={:.4}  \"{}\"", r.doc_id, r.score, title);
    }
}
