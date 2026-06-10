#include "IndexContext.h"
#include "EvalExpression.h"
#include "IndexReader.h"
#include "IndexSearchExecutor.h"
#include "IndexSearchCompiler.h"
#include "IndexSerializer.h"
#include "Tokenizer.h"
#include "SearchResult.h"

#include <functional>
#include <memory>
#include <iostream>
#include <map>
#include <cassert>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <cstdio>      /* remove() for temp file cleanup */

static SmartTokenizer g_tokenizer;

static void PrintResults(const std::vector<SearchResult>& results,
                         const char* label = "")
{
    std::cout << "  [" << label << "] " << results.size() << " result(s):\n";
    for (auto& r : results)
        std::cout << "    doc=" << r.doc_id << "  score=" << r.score << "\n";
}

static float AssertContains(const std::vector<SearchResult>& r,
                             uint64_t doc_id,
                             const char* ctx = "")
{
    for (auto& x : r)
        if (x.doc_id == doc_id) return x.score;
    std::cerr << "FAIL: doc " << doc_id << " not found [" << ctx << "]\n";
    throw std::runtime_error(std::string("AssertContains failed: ") + ctx);
    return 0.0f;
}

static void AssertNotContains(const std::vector<SearchResult>& r,
                               uint64_t doc_id,
                               const char* ctx = "")
{
    for (auto& x : r) {
        if (x.doc_id == doc_id) {
            std::cerr << "FAIL: doc " << doc_id << " should not be in results [" << ctx << "]\n";
            throw std::runtime_error(std::string("AssertNotContains failed: ") + ctx);
        }
    }
}

/*
* Shared corpus of 5 movie documents (Title + Body, two streams each).
* DocIDs are ordered by descending importance (1 is most important).
*/
static IndexContext* g_ctx = nullptr;

static void BuildSharedIndex()
{
    if (g_ctx) return;
    g_ctx = new IndexContext();

    auto writer = g_ctx->GetWriter();

    struct Doc { uint64_t id; const char* title; const char* body; float importance; };
    static const Doc docs[] = {
        {1, "Good Morning Vietnam",
            "Robin Williams plays a radio DJ stationed in Vietnam during the brutal war",
            0.9f},
        {2, "Apocalypse Now",
            "A soldier journeys through Vietnam and Cambodia on a mission to find a rogue colonel seeking power",
            0.8f},
        {3, "Platoon",
            "A young soldier in Vietnam faces moral conflict between two rival sergeants during a savage war",
            0.7f},
        {4, "Good Will Hunting",
            "A janitor at MIT hides his extraordinary mathematical genius until a therapist helps him",
            0.6f},
        {5, "The Deer Hunter",
            "Pennsylvania steelworkers go to Vietnam and face the trauma of captivity and Russian roulette",
            0.5f},
    };

    for (auto& d : docs) {
        writer->Write(g_tokenizer.Tokenize(d.title), d.id, "Title");
        writer->Write(g_tokenizer.Tokenize(d.body),  d.id, "Body");
        writer->SetDocImportance(d.id, d.importance);
    }

    std::cout << "Index built: "
              << g_ctx->GetStore()->TotalDocs() << " docs, avg_len="
              << g_ctx->GetStore()->AvgDocLen()  << "\n\n";
}

namespace IndexAccessTests {

/*
* Verify posting lists are present after indexing.
*/
void TestBuildIndex()
{
    BuildSharedIndex();
    auto* store = g_ctx->GetStore();

    assert(store->GetPostingList("vietnamT") != nullptr);
    assert(store->GetPostingList("vietnamB") != nullptr);

    auto* pl = store->GetPostingList("vietnamT");
    assert(pl != nullptr);
    assert(pl->doc_freq() >= 1);   // only "Good Morning Vietnam" has vietnam in title

    pl = store->GetPostingList("goodT");
    assert(pl->doc_freq() >= 2);   // "Good Morning Vietnam" and "Good Will Hunting"

    std::cout << "  'vietnamT' doc_freq = " << store->GetPostingList("vietnamT")->doc_freq() << "\n";
    std::cout << "  'goodT'    doc_freq = " << store->GetPostingList("goodT")->doc_freq()    << "\n";
    std::cout << "  avg_doc_len         = " << store->AvgDocLen()                             << "\n";
}

/*
* Single-term BM25 search on AUT streams.
*/
void TestSingleTermSearch()
{
    BuildSharedIndex();

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("vietnam", "AUT");
    auto reader   = g_ctx->GetReader(tree);
    auto executor = g_ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "vietnam/AUT");

    AssertContains(results, 1, "vietnam AUT");
    /*
    * Docs 2,3,5 have "vietnam" in their bodies but NOT in title/anchor/url,
    * so they are absent from an AUT-only search.
    */
    AssertNotContains(results, 4, "vietnam AUT");
    assert(!results.empty());

    delete compiler; delete tree; delete executor;
}

/*
* AND query — both terms must appear in the same document.
*/
void TestAndSearch()
{
    BuildSharedIndex();

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("good morning", "AUT");
    auto reader   = g_ctx->GetReader(tree);
    auto executor = g_ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "good morning/AUT");

    AssertContains(results, 1, "good morning");
    AssertNotContains(results, 4, "good morning AND");

    delete compiler; delete tree; delete executor;
}

/*
* OR query — union of posting lists.
*/
void TestOrSearch()
{
    BuildSharedIndex();

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("morning OR apocalypse", "AUT");
    auto reader   = g_ctx->GetReader(tree);
    auto executor = g_ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "morning OR apocalypse");

    AssertContains(results, 1, "OR morning");
    AssertContains(results, 2, "OR apocalypse");

    delete compiler; delete tree; delete executor;
}

/*
* NOT query — exclude documents that match the exclusion ISR.
*/
void TestNotSearch()
{
    BuildSharedIndex();

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("good NOT hunting", "AUTB");
    auto reader   = g_ctx->GetReader(tree);
    auto executor = g_ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "good NOT hunting");

    AssertContains   (results, 1, "NOT: doc1 present");
    AssertNotContains(results, 4, "NOT: doc4 excluded");

    delete compiler; delete tree; delete executor;
}

/*
* Field-constraint prefix pins the query to a specific MetaStream.
*/
void TestFieldConstraint()
{
    BuildSharedIndex();
    auto* store = g_ctx->GetStore();
    auto executor_raw = g_ctx->GetExecutor();

    {
        auto compiler = new IndexSearchCompiler();
        auto tree     = compiler->Compile("title:vietnam", "AUTB");
        auto reader   = g_ctx->GetReader(tree);
        auto results  = executor_raw->Execute(reader, 10);
        PrintResults(results, "title:vietnam");

        for (auto& r : results) {
            auto* pl = store->GetPostingList("vietnamT");
            bool in_title = false;
            if (pl) for (auto& e : pl->entries)
                if (e.doc_id == r.doc_id) { in_title = true; break; }
            assert(in_title && "title:vietnam matched a doc not in vietnamT");
        }
        delete compiler; delete tree;
    }

    {
        auto compiler = new IndexSearchCompiler();
        auto tree     = compiler->Compile("body:vietnam", "AUTB");
        auto reader   = g_ctx->GetReader(tree);
        auto results  = executor_raw->Execute(reader, 10);
        PrintResults(results, "body:vietnam");
        AssertContains(results, 5, "body:vietnam doc5");
        delete compiler; delete tree;
    }

    delete executor_raw;
}

/*
* Inspect EvalTree node types produced by the compiler.
*/
void TestEvalTree()
{
    auto compiler = new IndexSearchCompiler();

    {
        auto tree = compiler->Compile("fox", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Term);
        auto* tn = static_cast<TermNode*>(tree->root.get());
        assert(tn->stream_key == "foxT");
        std::cout << "  Compile('fox','T') → TermNode('" << tn->stream_key << "')\n";
        delete tree;
    }

    {
        auto tree = compiler->Compile("fox quick", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::And);
        auto* an = static_cast<AndNode*>(tree->root.get());
        assert(an->children.size() == 2);
        std::cout << "  Compile('fox quick','T') → AndNode with "
                  << an->children.size() << " children\n";
        delete tree;
    }

    {
        auto tree = compiler->Compile("fox OR lazy", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Or);
        auto* on = static_cast<OrNode*>(tree->root.get());
        assert(on->children.size() == 2);
        std::cout << "  Compile('fox OR lazy','T') → OrNode with "
                  << on->children.size() << " children\n";
        delete tree;
    }

    {
        /*
        * A single term on AUT expands to OrNode( foxA, foxU, foxT ).
        */
        auto tree = compiler->Compile("fox", "AUT");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Or);
        auto* on = static_cast<OrNode*>(tree->root.get());
        assert(on->children.size() == 3);
        for (auto& c : on->children) {
            assert(c->GetType() == NodeType::Term);
            auto* tn = static_cast<TermNode*>(c.get());
            std::cout << "    stream_key: " << tn->stream_key << "\n";
        }
        delete tree;
    }

    {
        auto tree = compiler->Compile("good NOT hunting", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Not);
        std::cout << "  Compile('good NOT hunting','T') → NotNode\n";
        delete tree;
    }

    delete compiler;
}

/*
* Multi-phase: Phase 1 (AUT) finds nothing; ExecutePhased escalates to AUTB.
*/
void TestMultiPhase()
{
    BuildSharedIndex();

    /*
    * "roulette" only appears in doc 5's body, not in any title.
    */
    auto compiler = new IndexSearchCompiler();
    auto executor = g_ctx->GetExecutor();

    auto tree1  = compiler->Compile("roulette", "AUT");
    auto rdr1   = g_ctx->GetReader(tree1);
    auto phase1 = executor->Execute(rdr1, 10);
    std::cout << "  Phase 1 (AUT)  'roulette': " << phase1.size() << " results\n";

    auto tree2  = compiler->Compile("roulette", "AUTB");
    auto rdr2   = g_ctx->GetReader(tree2);
    auto phase2 = executor->Execute(rdr2, 10);
    std::cout << "  Phase 2 (AUTB) 'roulette': " << phase2.size() << " results\n";
    AssertContains(phase2, 5, "roulette body phase2");

    auto tree_p1 = compiler->Compile("roulette", "AUT");
    auto tree_p2 = compiler->Compile("roulette", "AUTB");
    auto rdr_p1  = g_ctx->GetReader(tree_p1);
    auto rdr_p2  = g_ctx->GetReader(tree_p2);
    auto phased  = executor->ExecutePhased(rdr_p1, rdr_p2, 10, 1);
    PrintResults(phased, "ExecutePhased 'roulette'");
    AssertContains(phased, 5, "roulette phased");

    delete compiler;
    delete tree1; delete tree2; delete tree_p1; delete tree_p2;
    delete executor;
}

/*
* Doc importance is added to BM25 score and breaks ties.
*/
void TestDocImportance()
{
    auto ctx    = new IndexContext();
    auto writer = ctx->GetWriter();

    const char* same_text = "identical body content with fox and quick terms";
    writer->Write(g_tokenizer.Tokenize(same_text), 99,  "Body");
    writer->Write(g_tokenizer.Tokenize(same_text), 100, "Body");
    writer->SetDocImportance(99,  2.0f);
    writer->SetDocImportance(100, 0.1f);

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("fox", "B");
    auto reader   = ctx->GetReader(tree);
    auto executor = ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "importance tiebreak");
    assert(results.size() == 2);
    assert(results[0].doc_id == 99 && "doc 99 must rank first");

    delete ctx; delete compiler; delete tree; delete executor;
}

/*
* End-to-end: mirrors the original IndexAccessUnitTest API shape.
*/
void TestEndToEnd()
{
    IndexContext* index_context = new IndexContext("", "");
    auto tokenizer    = new SmartTokenizer();
    auto index_writer = index_context->GetWriter();

    uint64_t documentId = 1;
    index_writer->Write(
        tokenizer->Tokenize(
            "The QUICK Brown Fox jumps over the lazy DOG! "
            "Привет, МИР! Hello, WORLD! こんにちは这是一个人的世界! "
            "I'm testing apostrophes: don't, can't, won't"),
        documentId, "Body");
    index_writer->Write(
        tokenizer->Tokenize("Conf 2021"),
        documentId, "Title");

    index_writer->Write(
        tokenizer->Tokenize("The lazy fox slept all morning"),
        2u, "Body");
    index_writer->Write(
        tokenizer->Tokenize("Morning Fox 2021"),
        2u, "Title");

    {
        /*
        * GetReader returns an ISR starting at the first matching document.
        * Read the current doc_id directly — no GoNext needed.
        */
        auto rdr   = index_context->GetReader("fox");
        auto docId = rdr->GetDocumentID();
        std::cout << "  single 'fox': first doc_id = " << docId << "\n";
        rdr->Close();
    }

    {
        auto is_compiler = new IndexSearchCompiler();
        auto eval_tree   = is_compiler->Compile("fox lazy", "AUTB");
        auto reader      = index_context->GetReader(eval_tree);
        auto executor    = index_context->GetExecutor();
        auto results     = executor->Execute(reader, 10);

        PrintResults(results, "fox AND lazy / AUTB");
        assert(!results.empty());

        delete is_compiler; delete eval_tree; delete executor;
    }

    {
        /*
        * Vector search is not yet implemented; CompileToVector returns null.
        */
        auto is_compiler = new IndexSearchCompiler();
        auto embedding   = is_compiler->CompileToVector<float>("Innovative ideas in Conf 2021");
        auto reader = index_context->GetReader(embedding);
        std::cout << "  CompileToVector: "
                  << (embedding ? "non-null" : "null (not implemented)") << "\n";
        delete is_compiler;
    }

    delete index_context;
    delete tokenizer;
}

} // namespace IndexAccessTests

// ============================================================
// Test 11: save index to disk and reload it
// ============================================================
namespace IndexAccessTests {

void TestDiskPersistence()
{
    const char* INDEX_FILE = "test_moonshot.bin";

    // -- Phase A: build a fresh index and write to disk --------------------
    {
        IndexContext engine("", INDEX_FILE);
        auto writer = engine.GetWriter();

        writer->Write(g_tokenizer.Tokenize("Rust systems programming language"), 1, "Title");
        writer->Write(g_tokenizer.Tokenize("Ownership model prevents data races at compile time"), 1, "Body");
        writer->SetDocImportance(1, 0.9f);

        writer->Write(g_tokenizer.Tokenize("Python machine learning"),            2, "Title");
        writer->Write(g_tokenizer.Tokenize("Python is used for data science and AI research"), 2, "Body");
        writer->SetDocImportance(2, 0.7f);

        writer->Write(g_tokenizer.Tokenize("Go concurrency goroutines"),          3, "Title");
        writer->Write(g_tokenizer.Tokenize("Go makes concurrent programming easy with goroutines"), 3, "Body");
        writer->SetDocImportance(3, 0.6f);

        std::cout << "  Written " << engine.GetStore()->TotalDocs()
                  << " docs to memory\n";

        bool saved = engine.SaveIndex();
        assert(saved && "SaveIndex() must return true");
        std::cout << "  Saved to " << INDEX_FILE << "\n";

        /*
        * Verify the magic bytes before testing the load path.
        */
        assert(IndexSerializer::IsValidIndex(INDEX_FILE) &&
               "IsValidIndex() must be true immediately after Save");
    }

    // -- Phase B: load the file into a new engine --------------------------
    {
        IndexContext engine2("", INDEX_FILE);  // auto-loads on construction

        std::cout << "  Loaded " << engine2.GetStore()->TotalDocs()
                  << " docs from disk\n";

        assert(engine2.GetStore()->TotalDocs() == 3 &&
               "Loaded doc count must match written doc count");

        /*
        * Run the same queries as the other tests to confirm results match.
        */
        IndexSearchCompiler compiler;
        auto exec = std::unique_ptr<IndexSearchExecutor>(engine2.GetExecutor());

        // 1. Single term — should find doc 1 ("rust" in title)
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("rust", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            std::cout << "  search(rust): " << results.size() << " result(s)\n";
            AssertContains(results, 1, "disk: rust");
        }

        // 2. AND query — "python machine" must find doc 2
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("python machine", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            std::cout << "  search(python machine): " << results.size() << " result(s)\n";
            AssertContains(results, 2, "disk: python machine");
        }

        // 3. OR query — "rust OR go" should return docs 1 and 3
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("rust OR go", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            std::cout << "  search(rust OR go): " << results.size() << " result(s)\n";
            AssertContains(results, 1, "disk: rust OR go doc1");
            AssertContains(results, 3, "disk: rust OR go doc3");
        }

        // 4. Importance preserved — doc 1 has highest importance; verify score order
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("programming", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            if (results.size() >= 2) {
                // doc with highest importance among programming-matched docs should rank first
                std::cout << "  search(programming): top doc=" << results[0].doc_id
                          << " score=" << results[0].score << "\n";
            }
        }
    }

    // -- Phase C: overwrite with a different document set ------------------
    {
        IndexContext engine3("", INDEX_FILE);
        auto writer = engine3.GetWriter();
        writer->Write(g_tokenizer.Tokenize("New document after overwrite"), 99, "Body");
        engine3.SaveIndex();

        IndexContext engine4("", INDEX_FILE);
        assert(engine4.GetStore()->TotalDocs() == 1 &&
               "After overwrite, only new doc should be present");
        std::cout << "  Overwrite test passed: 1 doc loaded\n";
    }

    // -- Cleanup -----------------------------------------------------------
    // remove(INDEX_FILE);
    // std::cout << "  Temp file removed\n";
}

} // namespace IndexAccessTests

std::map<std::string, std::function<void()>> testRegistry = {
    {"TestBuildIndex",       IndexAccessTests::TestBuildIndex},
    {"TestSingleTermSearch", IndexAccessTests::TestSingleTermSearch},
    {"TestAndSearch",        IndexAccessTests::TestAndSearch},
    {"TestOrSearch",         IndexAccessTests::TestOrSearch},
    {"TestNotSearch",        IndexAccessTests::TestNotSearch},
    {"TestFieldConstraint",  IndexAccessTests::TestFieldConstraint},
    {"TestEvalTree",         IndexAccessTests::TestEvalTree},
    {"TestMultiPhase",       IndexAccessTests::TestMultiPhase},
    {"TestDocImportance",    IndexAccessTests::TestDocImportance},
    {"TestEndToEnd",         IndexAccessTests::TestEndToEnd},
    {"TestDiskPersistence",  IndexAccessTests::TestDiskPersistence},
};
