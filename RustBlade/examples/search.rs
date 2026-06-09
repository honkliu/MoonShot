/// RustBlade — end-to-end usage example.
///
/// Shows the three search modes in order of complexity:
///   1. Full-text (BM25)
///   2. Vector KNN (HNSW cosine)
///   3. Hybrid (text + vector fused with RRF)
///
/// Run with:   cargo run --example search

use rustblade::{Document, Engine, EngineConfig, Metric};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // -----------------------------------------------------------------------
    // 1. Build the engine with an explicit vector field config.
    //    For demo purposes we also rely on auto-detection (the engine picks up
    //    any Vec<f32> field not listed in the config automatically).
    // -----------------------------------------------------------------------
    let config = EngineConfig::default()
        .with_hnsw_field("embedding", 4, Metric::Cosine);

    let mut engine = Engine::with_config(config);

    // -----------------------------------------------------------------------
    // 2. Index documents.
    //    Each Document can carry text fields (full-text indexed + stored),
    //    vector fields (HNSW indexed), and integer / float fields (stored).
    // -----------------------------------------------------------------------
    engine.add_document(
        Document::new(1)
            .with_text("title", "Rust Programming Language")
            .with_text("body", "Rust is a systems programming language focused on safety speed and concurrency")
            .with_vector("embedding", vec![0.10, 0.20, 0.30, 0.40]),
    )?;

    engine.add_document(
        Document::new(2)
            .with_text("title", "Python Machine Learning")
            .with_text("body", "Python is widely used for machine learning data science and AI research")
            .with_vector("embedding", vec![0.50, 0.60, 0.10, 0.20]),
    )?;

    engine.add_document(
        Document::new(3)
            .with_text("title", "Go Concurrency Patterns")
            .with_text("body", "Go makes it easy to write concurrent programs with goroutines and channels")
            .with_vector("embedding", vec![0.15, 0.25, 0.35, 0.45]),
    )?;

    engine.add_document(
        Document::new(4)
            .with_text("title", "Rust Memory Safety")
            .with_text("body", "Rust prevents data races and memory errors at compile time using ownership rules")
            .with_vector("embedding", vec![0.12, 0.22, 0.32, 0.42]),
    )?;

    engine.add_document(
        Document::new(5)
            .with_text("title", "Vector Database Design")
            .with_text("body", "Modern vector databases use HNSW for approximate nearest neighbour search at scale")
            .with_vector("embedding", vec![0.80, 0.05, 0.90, 0.01]),
    )?;

    // -----------------------------------------------------------------------
    // 3. Build indexes (compile postings, freeze HNSW graph).
    // -----------------------------------------------------------------------
    engine.build()?;

    println!("Indexed {} documents\n", engine.doc_count());

    // -----------------------------------------------------------------------
    // 4. Full-text search (BM25).
    // -----------------------------------------------------------------------
    println!("=== Full-text: \"rust safety\" ===");
    let results = engine.search("rust safety", 5)?;
    print_results(&results);

    println!("\n=== Full-text: \"machine learning python\" ===");
    let results = engine.search("machine learning python", 5)?;
    print_results(&results);

    println!("\n=== Full-text OR syntax: \"rust OR go\" ===");
    let results = engine.search("rust OR go", 5)?;
    print_results(&results);

    println!("\n=== Full-text NOT syntax: \"rust NOT memory\" ===");
    let results = engine.search("rust NOT memory", 5)?;
    print_results(&results);

    // -----------------------------------------------------------------------
    // 5. KNN vector search.
    // -----------------------------------------------------------------------
    let query_vec = vec![0.12, 0.21, 0.31, 0.41];   // close to Rust docs
    println!("\n=== KNN vector search (field=\"embedding\", k=3) ===");
    let results = engine.search_knn("embedding", &query_vec, 3)?;
    print_results(&results);

    // -----------------------------------------------------------------------
    // 6. Hybrid search (text + vector fused with RRF).
    // -----------------------------------------------------------------------
    println!("\n=== Hybrid: text=\"safety\" + vector close to Rust docs ===");
    let results = engine.search_hybrid("safety", "embedding", &query_vec, 5)?;
    print_results(&results);

    // -----------------------------------------------------------------------
    // 7. Persist and reload.
    // -----------------------------------------------------------------------
    let path = std::env::temp_dir().join("rustblade_demo.json");
    let path = path.to_str().expect("temp dir path");
    engine.save(path)?;
    println!("\nSaved index to {path}");

    let mut engine2 = Engine::load(path)?;
    println!("Reloaded index ({} docs)\n", engine2.doc_count());

    println!("=== Search after reload: \"concurrency\" ===");
    let results = engine2.search("concurrency", 5)?;
    print_results(&results);

    Ok(())
}

fn print_results(results: &[rustblade::SearchResult]) {
    if results.is_empty() {
        println!("  (no results)");
        return;
    }
    for r in results {
        let title = r.fields.get("title").map(String::as_str).unwrap_or("—");
        println!("  [doc {}]  score={:.4}  title=\"{}\"", r.doc_id, r.score, title);
    }
}
