/*
 * fulltext_search.cpp — comprehensive MoonShot SDK demonstration.
 *
 * Covers:
 *   - Indexing with multiple MetaStreams (Title, Body)
 *   - Boolean query syntax (AND, OR, NOT, field constraints)
 *   - Multi-phase search (AUT → AUTB fallback)
 *   - Document importance scoring
 *   - Save/load index to and from disk
 *   - Manual EvalTree construction
 *   - Low-level ISR access
 *
 * Build:
 *   cmake -B build && cmake --build build --target fulltext_search
 *   ./build/examples/fulltext_search
 */

#include "moonshot.h"
#include <print>
#include <vector>
#include <string>

/* ========================================================================= *
 * Corpus                                                                      *
 * ========================================================================= */
struct Article {
    uint64_t    id;
    const char* title;
    const char* body;
    float       importance;
};

static const Article CORPUS[] = {
    {1, "Rust Programming Language",
        "Rust is a systems language focused on safety and zero-cost abstractions. "
        "The ownership model prevents data races at compile time.",
        0.95f},
    {2, "Python for Data Science",
        "Python is widely used for machine learning and data science. "
        "Libraries such as NumPy, Pandas, and PyTorch make it powerful.",
        0.80f},
    {3, "Go Concurrency Patterns",
        "Go makes concurrent programming easy with goroutines and channels. "
        "The runtime scheduler multiplexes goroutines onto OS threads.",
        0.70f},
    {4, "Memory Safety in C++",
        "C++ offers manual memory management but risks dangling pointers and "
        "buffer overflows. Smart pointers and RAII mitigate many issues.",
        0.60f},
    {5, "Functional Programming with Haskell",
        "Haskell enforces purity and uses lazy evaluation. "
        "Monads model effectful computations in a pure language.",
        0.55f},
    {6, "Database Indexing Techniques",
        "B-trees and LSM-trees are the dominant structures for disk-based "
        "database indexes. Inverted indexes power full-text search.",
        0.75f},
};

/* ========================================================================= *
 * Helpers                                                                     *
 * ========================================================================= */
static void printResults(const std::vector<SearchResult>& results,
                          const Article* corpus, size_t corpus_size)
{
    if (results.empty()) { std::println("  (no results)"); return; }
    for (auto& r : results) {
        const char* title = "—";
        for (size_t i = 0; i < corpus_size; ++i)
            if (corpus[i].id == r.doc_id) { title = corpus[i].title; break; }
        std::println("  [{}] score={:.3f}  \"{}\"", r.doc_id, r.score, title);
    }
}
#define PRINT(results) printResults(results, CORPUS, sizeof(CORPUS)/sizeof(CORPUS[0]))

static std::vector<SearchResult>
run(IndexContext& engine, IndexSearchCompiler& compiler,
    const char* query, const char* streams = "AUTB", int top = 5)
{
    std::unique_ptr<EvalTree>            tree(compiler.Compile(query, streams));
    std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());
    return exec->Execute(engine.GetReader(tree.get()), top);
}

/* ========================================================================= *
 * Main                                                                        *
 * ========================================================================= */
int main()
{
    const char* INDEX_PATH = "fulltext.bin";

    /* ------------------------------------------------------------------ *
     * 1.  Build the index.                                                 *
     * ------------------------------------------------------------------ */
    std::println("=== Building index ===");
    {
        IndexContext   engine("", INDEX_PATH);
        SmartTokenizer tok;
        auto           writer = engine.GetWriter();

        for (const auto& a : CORPUS) {
            writer->Write(tok.Tokenize(a.title), a.id, "Title");
            writer->Write(tok.Tokenize(a.body),  a.id, "Body");
            writer->SetDocImportance(a.id, a.importance);
        }

        std::println("  {} documents indexed  (avg_len={:.1f} tokens)",
                     engine.GetStore()->TotalDocs(),
                     engine.GetStore()->AvgDocLen());

        engine.SaveIndex();
        std::println("  Index saved to {}\n", INDEX_PATH);
    }

    /* ------------------------------------------------------------------ *
     * 2.  Load from disk and run queries.                                  *
     * ------------------------------------------------------------------ */
    std::println("=== Loading index from disk ===");
    IndexContext        engine("", INDEX_PATH);
    IndexSearchCompiler compiler;
    std::println("  {} documents loaded\n", engine.GetStore()->TotalDocs());

    std::println("Query: rust");
    PRINT(run(engine, compiler, "rust"));

    std::println("\nQuery: memory safety");
    PRINT(run(engine, compiler, "memory safety"));

    std::println("\nQuery: rust programming");
    PRINT(run(engine, compiler, "rust programming"));

    std::println("\nQuery: python data science");
    PRINT(run(engine, compiler, "python data science"));

    /* ------------------------------------------------------------------ *
     * 3.  Multi-phase search.                                              *
     *     Phase 1 (AUT) targets Title, URL, Anchor.                       *
     *     ExecutePhased falls back to Phase 2 (AUTB) when < min results.  *
     * ------------------------------------------------------------------ */
    std::println("\n=== Multi-phase search ===");
    {
        auto tree1 = std::unique_ptr<EvalTree>(compiler.Compile("inverted", "AUT"));
        auto tree2 = std::unique_ptr<EvalTree>(compiler.Compile("inverted", "AUTB"));
        auto exec  = std::unique_ptr<IndexSearchExecutor>(engine.GetExecutor());

        auto phased = exec->ExecutePhased(engine.GetReader(tree1.get()),
                                          engine.GetReader(tree2.get()),
                                          5, /*min_before_fallback=*/ 1);
        std::println("  ExecutePhased(\"inverted\", min=1):");
        PRINT(phased);
    }

    /* ------------------------------------------------------------------ *
     * 4.  Document importance — two docs with identical text, different   *
     *     importance values; the higher one must rank first.              *
     * ------------------------------------------------------------------ */
    std::println("\n=== Importance-based ranking ===");
    {
        IndexContext   local;
        SmartTokenizer tok;
        auto           w   = local.GetWriter();
        w->Write(tok.Tokenize("identical content about search"), 10, "Body");
        w->Write(tok.Tokenize("identical content about search"), 11, "Body");
        w->SetDocImportance(10, 0.9f);
        w->SetDocImportance(11, 0.1f);

        std::unique_ptr<EvalTree>            t(compiler.Compile("search", "B"));
        std::unique_ptr<IndexSearchExecutor> e(local.GetExecutor());
        auto results = e->Execute(local.GetReader(t.get()), 5);

        std::println("  doc={} (importance=0.9) score={:.3f}  [must be first]",
                     results[0].doc_id, results[0].score);
        std::println("  doc={} (importance=0.1) score={:.3f}",
                     results[1].doc_id, results[1].score);
    }

    /* ------------------------------------------------------------------ *
     * 5.  Low-level ISR access.                                           *
     * ------------------------------------------------------------------ */
    std::println("\n=== Low-level ISR iteration ===");
    {
        auto reader = engine.GetReader("rust", "AUT");
        Bm25Scorer scorer(engine.GetStore()->TotalDocs(),
                          engine.GetStore()->AvgDocLen());
        std::println("  Posting list for \"rust\" (across AUT streams):");
        while (!reader->IsEnd()) {
            uint64_t doc = reader->GetDocumentID();
            uint32_t tf  = reader->GetTermFreq();
            uint32_t dl  = engine.GetStore()->GetDocLen(doc);
            float    bm  = reader->GetBM25Score(scorer, dl);
            std::println("    doc={:<3}  tf={}  bm25={:.3f}", doc, tf, bm);
            reader->GoNext();
        }
    }

    /* ------------------------------------------------------------------ *
     * 6.  Manual EvalTree construction.                                   *
     * ------------------------------------------------------------------ */
    std::println("\n=== Manual EvalTree: AND(rustT, safetyB) ===");
    {
        auto and_node = std::make_shared<AndNode>();
        and_node->children.push_back(std::make_shared<TermNode>("rustT"));
        and_node->children.push_back(std::make_shared<TermNode>("safetyB"));

        EvalTree tree;
        tree.root = and_node;

        std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());
        auto results = exec->Execute(engine.GetReader(&tree), 5);
        PRINT(results);
    }

    std::println("\nDone.");
    return 0;
}
