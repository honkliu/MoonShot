/*
* demo.rs — end-to-end RustBlade example.
*
* BuildIndex : tokenize five documents and save the index to disk.
* SearchIndex: reload from disk, compile queries, rank with BM25.
*
*   cargo run --example demo -- -b -q
*/
use rustblade::{IndexContext, IndexWriter, SmartTokenizer};
use rustblade::tokenizer::Tokenizer;
use rustblade::compiler::IndexSearchCompiler;
use rustblade::executor::{IndexSearchExecutor, SearchResult};

static INDEX_FILE: &str = "moonshot_demo.idx";

struct Doc {
    id:         u64,
    anchor:     &'static str,
    url:        &'static str,
    title:      &'static str,
    body:       &'static str,
    importance: f32,
}

static DOCS: &[Doc] = &[
    Doc { id: 1,
        anchor: "Honda Race Car Toy",
        url:    "www.honda.com/toys/race-car",
        title:  "Honda Miniature Race Car Toy",
        body:   "Honda's official die-cast miniature race car toy for collectors. \
                 Precision-engineered scale model of the Honda RC213V MotoGP race car. \
                 Suitable for racing enthusiasts and children aged 8 and up.",
        importance: 1.2 },
    Doc { id: 2,
        anchor: "Honda Civic Sport",
        url:    "www.honda.com/civic/sport",
        title:  "Honda Civic Sport 2024",
        body:   "The Honda Civic Sport delivers turbocharged performance with a sleek \
                 design. Fuel-efficient engine, adaptive suspension, and Apple CarPlay. \
                 The ideal everyday sports car for modern drivers.",
        importance: 1.0 },
    Doc { id: 3,
        anchor: "Toyota Race Car Collection",
        url:    "www.toyota.com/motorsport/collection",
        title:  "Toyota Motorsport Race Car Models",
        body:   "Official Toyota GR motorsport race car scale models and toy collection. \
                 Includes the Toyota GR010 Le Mans hypercar and GR Yaris rally car toys. \
                 Great gifts for racing fans of all ages.",
        importance: 0.9 },
    Doc { id: 4,
        anchor: "Best Toy Cars 2024",
        url:    "www.google.com/shopping/toy-cars",
        title:  "Top Toy Race Cars 2024 Google Shopping",
        body:   "Google Shopping curated list of the best toy race cars available today. \
                 Includes die-cast models from Honda, Toyota, Ferrari and Hot Wheels. \
                 Compare prices and read reviews before you buy.",
        importance: 0.7 },
    Doc { id: 5,
        anchor: "Formula One Engineering Guide",
        url:    "www.motorsport.org/f1-guide",
        title:  "Formula One Race Car Aerodynamics",
        body:   "A technical guide to Formula One race car design and aerodynamics. \
                 Covers wing configuration, downforce, tire compounds, and pit strategy. \
                 Not affiliated with Honda, Toyota or any other manufacturer.",
        importance: 0.6 },
];

fn build_index() {
    println!("\n=== BuildIndex ===");

    let mut ctx    = IndexContext::new();
    let tok        = SmartTokenizer::new();
    let mut writer = ctx.get_writer();

    for doc in DOCS {
        writer.write(tok.tokenize(doc.anchor), doc.id, "Anchor");
        writer.write(tok.tokenize(doc.url),    doc.id, "URL");
        writer.write(tok.tokenize(doc.title),  doc.id, "Title");
        writer.write(tok.tokenize(doc.body),   doc.id, "Body");
        writer.set_doc_importance(doc.id, doc.importance);
        println!("  indexed doc {}: {}", doc.id, doc.title);
    }

    ctx.save_index(INDEX_FILE).expect("save failed");
    println!("  saved → {}", INDEX_FILE);
}

fn doc_title(id: u64) -> &'static str {
    DOCS.iter().find(|d| d.id == id).map(|d| d.title).unwrap_or("(unknown)")
}

fn search_index() {
    println!("\n=== SearchIndex ===");

    let mut ctx      = IndexContext::with_path(Some(INDEX_FILE.to_string()));
    let compiler     = IndexSearchCompiler::new(SmartTokenizer::new());

    let run = |ctx: &mut IndexContext, q: &str| {
        let tree    = compiler.compile(q, "AUTB");
        let mut reader = ctx.get_reader(tree);
        let hits    = ctx.with_store(|store| {
            let exec = IndexSearchExecutor::new(store);
            exec.execute(reader.as_mut(), 10)
        });
        println!("\n[{}]", q);
        for h in &hits {
            println!("  doc {:<3}  score={:.3}  {}", h.doc_id, h.score, doc_title(h.doc_id));
        }
    };

    run(&mut ctx, "honda");
    run(&mut ctx, "race car");
    run(&mut ctx, "race car honda");
    run(&mut ctx, "toy race car");
    run(&mut ctx, "honda toyota");

    /* debug trace */
    println!("\n--- trace: race car toy ---");
    {
        let tree        = compiler.compile("race car toy", "AUTB");
        let mut reader  = ctx.get_reader(tree);
        reader.set_debug("race car toy", 0);

        let hits = ctx.with_store(|store| {
            let exec = IndexSearchExecutor::new(store);
            exec.execute(reader.as_mut(), 10)
        });

        println!("\n  results:");
        for h in &hits {
            println!("    doc {:<3}  score={:.3}", h.doc_id, h.score);
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let do_build  = args.iter().any(|a| a == "-b");
    let do_search = args.iter().any(|a| a == "-q");

    if !do_build && !do_search {
        println!("usage: demo -b       build index");
        println!("            -q       search index");
        println!("            -b -q    build then search");
        return;
    }

    if do_build  { build_index(); }
    if do_search { search_index(); }
}
