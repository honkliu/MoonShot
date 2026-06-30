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
        std::cout << "    doc=" << ReaderDocumentIDValue(r.doc_id) << "  score=" << r.score << "\n";
}

static float AssertContains(const std::vector<SearchResult>& r,
                             uint64_t doc_id,
                             const char* ctx = "")
{
    for (auto& x : r)
        if (ReaderDocumentIDValue(x.doc_id) == doc_id) return x.score;
    std::cerr << "FAIL: doc " << doc_id << " not found [" << ctx << "]\n";
    throw std::runtime_error(std::string("AssertContains failed: ") + ctx);
    return 0.0f;
}

static void AssertNotContains(const std::vector<SearchResult>& r,
                               uint64_t doc_id,
                               const char* ctx = "")
{
    for (auto& x : r) {
        if (ReaderDocumentIDValue(x.doc_id) == doc_id) {
            std::cerr << "FAIL: doc " << doc_id << " should not be in results [" << ctx << "]\n";
            throw std::runtime_error(std::string("AssertNotContains failed: ") + ctx);
        }
    }
}

/*
* Shared corpus of 5 movie documents (Title + Body, two streams each).
* DocIDs are ordered by descending importance (0 is most important).
*/
static IndexContext* g_ctx = nullptr;

static void BuildSharedIndex()
{
    if (g_ctx) return;
    g_ctx = new IndexContext();

    auto writer = g_ctx->GetWriter();

    struct Doc { uint64_t id; const char* title; const char* body; float importance; };
    static const Doc docs[] = {
        {0, "Good Morning Vietnam",
            "Robin Williams plays a radio DJ stationed in Vietnam during the brutal war",
            0.9f},
        {1, "Apocalypse Now",
            "A soldier journeys through Vietnam and Cambodia on a mission to find a rogue colonel seeking power",
            0.8f},
        {2, "Platoon",
            "A young soldier in Vietnam faces moral conflict between two rival sergeants during a savage war",
            0.7f},
        {3, "Good Will Hunting",
            "A janitor at MIT hides his extraordinary mathematical genius until a therapist helps him",
            0.6f},
        {4, "The Deer Hunter",
            "Pennsylvania steelworkers go to Vietnam and face the trauma of captivity and Russian roulette",
            0.5f},
    };

    for (auto& d : docs) {
        writer->Write(g_tokenizer.Tokenize(d.title), d.id, "Title");
        writer->Write(g_tokenizer.Tokenize(d.body),  d.id, "Body");
        writer->SetDocImportance(d.id, d.importance);
        writer->SetDocVector(d.id, BuildHashedEmbedding(g_tokenizer.Tokenize((std::string(d.title) + " " + d.body).c_str())));
    }
    g_ctx->Build();

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

    auto* postingList = store->GetPostingList("vietnamT");
    assert(postingList != nullptr);
    assert(postingList->doc_freq() >= 1);

    postingList = store->GetPostingList("goodT");
    assert(postingList->doc_freq() >= 2);

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

    AssertContains(results, 0, "vietnam AUT");
    /*
    * Docs 1,2,4 have "vietnam" in their bodies but NOT in title/anchor/url,
    * so they are absent from an AUT-only search.
    */
    AssertNotContains(results, 3, "vietnam AUT");
    assert(!results.empty());

    delete compiler; delete tree;
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

    AssertContains(results, 0, "good morning");
    AssertNotContains(results, 3, "good morning AND");

    delete compiler; delete tree;
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

    AssertContains(results, 0, "OR morning");
    AssertContains(results, 1, "OR apocalypse");

    delete compiler; delete tree;
}

void TestWeakAndSearch()
{
    IndexContext engine;
    SmartTokenizer tok;
    auto writer = engine.GetWriter();

    writer->Write(tok.Tokenize("alpha beta gamma"), 0, "Title");
    writer->SetDocImportance(0, 0.1f);
    writer->SetDocVector(0, BuildHashedEmbedding(tok.Tokenize("alpha beta gamma")));

    writer->Write(tok.Tokenize("alpha beta"), 1, "Title");
    writer->SetDocImportance(1, 0.1f);
    writer->SetDocVector(1, BuildHashedEmbedding(tok.Tokenize("alpha beta")));

    writer->Write(tok.Tokenize("alpha"), 2, "Title");
    writer->SetDocImportance(2, 0.1f);
    writer->SetDocVector(2, BuildHashedEmbedding(tok.Tokenize("alpha")));

    engine.Build();

    std::vector<std::shared_ptr<IndexReader>> children;
    children.push_back(engine.GetStreamReader("alphaT"));
    children.push_back(engine.GetStreamReader("betaT"));
    children.push_back(engine.GetStreamReader("gammaT"));

    auto reader = std::make_shared<WeakAndIndexReader>(std::move(children), 2);
    std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());
    auto results = exec->Execute(reader, 10);

    AssertContains(results, 0, "weak-and doc with 3 terms");
    AssertContains(results, 1, "weak-and doc with 2 terms");
    AssertNotContains(results, 2, "weak-and excludes doc with 1 term");
    std::cout << "  weak-and min=2 returned docs with at least two matching terms\n";
}

/*
* Minus query — exclude documents that match the exclusion ISR.
*/
void TestNotSearch()
{
    BuildSharedIndex();

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("good -hunting", "AUTB");
    auto reader   = g_ctx->GetReader(tree);
    auto executor = g_ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "good -hunting");

    AssertContains   (results, 0, "NOT: doc0 present");
    AssertNotContains(results, 3, "NOT: doc3 excluded");

    delete compiler; delete tree;
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

        for (auto& result : results) {
            const PostingList* postingList = store->GetPostingList("vietnamT");
            bool inTitle = false;
            if (postingList)
                for (auto& entry : postingList->entries)
                    if (entry.IE_DocID == ReaderDocumentIDValue(result.doc_id)) { inTitle = true; break; }
            assert(inTitle && "title:vietnam matched a doc not in vietnamT");
        }
        delete compiler; delete tree;
    }

    {
        auto compiler = new IndexSearchCompiler();
        auto tree     = compiler->Compile("body:vietnam", "AUTB");
        auto reader   = g_ctx->GetReader(tree);
        auto results  = executor_raw->Execute(reader, 10);
        PrintResults(results, "body:vietnam");
        AssertContains(results, 4, "body:vietnam doc4");
        delete compiler; delete tree;
    }
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
        auto* termNode = static_cast<TermNode*>(tree->root.get());
        assert(termNode->stream_key == "foxT");
        std::cout << "  Compile('fox','T') → TermNode('" << termNode->stream_key << "')\n";
        delete tree;
    }

    {
        auto tree = compiler->Compile("fox quick", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Or);
        auto* orNode = static_cast<OrNode*>(tree->root.get());
        assert(orNode->children.size() == 2);
        assert(orNode->children[0]->GetType() == NodeType::Term);
        assert(orNode->children[1]->GetType() == NodeType::And);
        std::cout << "  Compile('fox quick','T') → OrNode(bigram, unigram AND)\n";
        delete tree;
    }

    {
        auto tree = compiler->Compile("fox OR lazy", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Or);
        auto* orNode = static_cast<OrNode*>(tree->root.get());
        assert(orNode->children.size() == 2);
        std::cout << "  Compile('fox OR lazy','T') → OrNode with "
                  << orNode->children.size() << " children\n";
        delete tree;
    }

    {
        /*
        * A single term on AUT expands to OrNode( foxA, foxU, foxT ).
        */
        auto tree = compiler->Compile("fox", "AUT");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Or);
        auto* orNode = static_cast<OrNode*>(tree->root.get());
        assert(orNode->children.size() == 3);
        for (auto& child : orNode->children) {
            assert(child->GetType() == NodeType::Term);
            auto* termNode = static_cast<TermNode*>(child.get());
            std::cout << "    stream_key: " << termNode->stream_key << "\n";
        }
        delete tree;
    }

    {
        auto tree = compiler->Compile("good -hunting", "T");
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Not);
        std::cout << "  Compile('good -hunting','T') → NotNode\n";
        delete tree;
    }

    delete compiler;
}

/*
* Field expansion: AUT misses body-only text; AUTB finds it.
*/
void TestMultiPhase()
{
    BuildSharedIndex();

    /*
    * "roulette" only appears in doc 4's body, not in any title.
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
    AssertContains(phase2, 4, "roulette body phase2");

    delete compiler;
    delete tree1; delete tree2;
}

/*
* Doc importance is added to BM25 score and breaks ties.
*/
void TestDocImportance()
{
    auto ctx    = new IndexContext();
    auto writer = ctx->GetWriter();

    const char* same_text = "identical body content with fox and quick terms";
    writer->Write(g_tokenizer.Tokenize(same_text), 0, "Body");
    writer->Write(g_tokenizer.Tokenize(same_text), 1, "Body");
    writer->SetDocImportance(0, 2.0f);
    writer->SetDocImportance(1, 0.1f);
    writer->SetDocVector(0, BuildHashedEmbedding(g_tokenizer.Tokenize(same_text)));
    writer->SetDocVector(1, BuildHashedEmbedding(g_tokenizer.Tokenize(same_text)));
    ctx->Build();

    auto compiler = new IndexSearchCompiler();
    auto tree     = compiler->Compile("fox", "B");
    auto reader   = ctx->GetReader(tree);
    auto executor = ctx->GetExecutor();
    auto results  = executor->Execute(reader, 10);

    PrintResults(results, "importance tiebreak");
    assert(results.size() == 2);
    assert(ReaderDocumentIDValue(results[0].doc_id) == 0 && "doc 0 must rank first");

    delete ctx; delete compiler; delete tree;
}

/*
* End-to-end: mirrors the original IndexAccessUnitTest API shape.
*/
void TestEndToEnd()
{
    IndexContext* index_context = new IndexContext("", "");
    auto tokenizer    = new SmartTokenizer();
    auto index_writer = index_context->GetWriter();

    uint64_t documentId = index_context->AllocateDocumentID();
    assert(documentId == 0);
    const std::string doc1Body =
        "The QUICK Brown Fox jumps over the lazy DOG! "
        "Привет, МИР! Hello, WORLD! こんにちは这是一个人的世界! "
        "I'm testing apostrophes: don't, can't, won't";
    auto doc1BodyTokens = tokenizer->Tokenize(doc1Body.c_str());
    auto doc1TitleTokens = tokenizer->Tokenize("Conf 2021");
    index_writer->Write(
        doc1BodyTokens,
        documentId, "Body");
    index_writer->Write(
        doc1TitleTokens,
        documentId, "Title");
    index_writer->SetDocVector(documentId,
        BuildHashedEmbedding(tokenizer->Tokenize("quick brown fox lazy dog")));

    Document doc2;
    doc2.title = "Morning Fox 2021";
    doc2.body = "The lazy fox slept all morning";
    uint64_t doc2Id = index_context->AddDocument(doc2);
    assert(doc2Id == 1);
    index_context->Build();

    {
        /*
        * GetReader returns an IndexReader starting at the first matching document.
        * Read the current doc_id directly — no GoNext needed.
        */
        auto rdr   = index_context->GetReader("fox", "AUT");
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
        std::unique_ptr<EvalTree> tree(index_context->Compile("fox lazy", "AUTBV"));
        assert(tree != nullptr);
        assert(tree->HasTextQuery());
        assert(tree->HasVectorQuery());
        assert(tree->vector_query.size() == DOC_VECTOR_DIM);
        auto reader = index_context->GetReader(tree.get());
        auto executor = index_context->GetExecutor();
        auto results = executor->Execute(reader, 10);
        assert(!results.empty());
        std::cout << "  Compile('fox lazy','AUTBV'): text + vector dim="
              << tree->vector_query.size()
              << ", results=" << results.size() << "\n";
        delete executor;
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

        writer->Write(g_tokenizer.Tokenize("Rust systems programming language"), 0, "Title");
        writer->Write(g_tokenizer.Tokenize("Ownership model prevents data races at compile time"), 0, "Body");
        writer->SetDocImportance(0, 0.9f);
        writer->SetDocVector(0, BuildHashedEmbedding(g_tokenizer.Tokenize("rust systems programming")));

        writer->Write(g_tokenizer.Tokenize("Python machine learning"),            1, "Title");
        writer->Write(g_tokenizer.Tokenize("Python is used for data science and AI research"), 1, "Body");
        writer->SetDocImportance(1, 0.7f);
        writer->SetDocVector(1, BuildHashedEmbedding(g_tokenizer.Tokenize("python machine learning")));

        writer->Write(g_tokenizer.Tokenize("Go concurrency goroutines"),          2, "Title");
        writer->Write(g_tokenizer.Tokenize("Go makes concurrent programming easy with goroutines"), 2, "Body");
        writer->SetDocImportance(2, 0.6f);
        writer->SetDocVector(2, BuildHashedEmbedding(g_tokenizer.Tokenize("go concurrency goroutines")));

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

        std::cout << "  Loaded " << engine2.DocumentCount()
              << " docs from disk\n";

        assert(engine2.DocumentCount() == 3 &&
               "Loaded doc count must match written doc count");
         engine2.Build();
         assert(engine2.VectorCount() == 3 &&
             "Loaded vector count must come from DocDataEntry vectors");
         auto vectorResults = engine2.VectorSearch(BuildHashedEmbedding(g_tokenizer.Tokenize("rust systems programming")), 3);
         bool foundVectorDoc0 = false;
         for (const auto& result : vectorResults)
             if (ReaderDocumentIDValue(result.doc_id) == 0) foundVectorDoc0 = true;
         assert(foundVectorDoc0 && "DocDataEntry vectors must rebuild IndexContext vector index");

        /*
        * Run the same queries as the other tests to confirm results match.
        */
        IndexSearchCompiler compiler;
        auto exec = engine2.GetExecutor();

        // 1. Single term — should find doc 0 ("rust" in title)
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("rust", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            std::cout << "  search(rust): " << results.size() << " result(s)\n";
            AssertContains(results, 0, "disk: rust");
        }

        // 2. AND query — "python machine" must find doc 1
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("python machine", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            std::cout << "  search(python machine): " << results.size() << " result(s)\n";
            AssertContains(results, 1, "disk: python machine");
        }

        // 3. OR query — "rust OR go" should return docs 0 and 2
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("rust OR go", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            std::cout << "  search(rust OR go): " << results.size() << " result(s)\n";
            AssertContains(results, 0, "disk: rust OR go doc0");
            AssertContains(results, 2, "disk: rust OR go doc2");
        }

        // 4. Importance preserved — doc 0 has highest importance; verify score order
        {
            auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("programming", "AUTB"));
            auto results = exec->Execute(engine2.GetReader(tree.get()), 5);
            if (results.size() >= 2) {
                // doc with highest importance among programming-matched docs should rank first
                std::cout << "  search(programming): top doc=" << ReaderDocumentIDValue(results[0].doc_id)
                          << " score=" << results[0].score << "\n";
            }
        }
    }

    // -- Phase C: overwrite with a different document set ------------------
    {
        IndexContext engine3("", INDEX_FILE);
        auto writer = engine3.GetWriter();
        writer->Write(g_tokenizer.Tokenize("New document after overwrite"), 0, "Body");
        writer->SetDocVector(0, BuildHashedEmbedding(g_tokenizer.Tokenize("new document overwrite")));
        engine3.SaveIndex();

        IndexContext engine4("", INDEX_FILE);
        assert(engine4.DocumentCount() == 1 &&
               "After overwrite, only new doc should be present");
        std::cout << "  Overwrite test passed: 1 doc loaded\n";
    }

    // -- Cleanup -----------------------------------------------------------
    // remove(INDEX_FILE);
    // std::cout << "  Temp file removed\n";
}

} // namespace IndexAccessTests

// ============================================================
// Test 12: save delta and read it without reloading from disk
// ============================================================
namespace IndexAccessTests {

void TestDeltaRuntimeHandoff()
{
    const char* INDEX_FILE = "test_moonshot_delta_base.bin";
    const char* DELTA_FILE = "test_moonshot_delta_base.delta.bin";
    std::remove(INDEX_FILE);
    std::remove(DELTA_FILE);

    {
        IndexContext base;
        Document doc;
        doc.doc_id = 0;
        doc.path = "base.txt";
        doc.title = "base apple";
        doc.body = "base document only";
        base.AddDocument(doc);
        assert(base.SaveIndex(INDEX_FILE));
    }

    {
        IndexContext engine("", INDEX_FILE);
        Document doc;
        doc.path = "delta.txt";
        doc.title = "delta banana";
        doc.body = "delta runtime document";
        const uint64_t deltaDocId = engine.AddDocument(doc);
        assert(deltaDocId == 1);

        assert(engine.SaveIndex(DELTA_FILE));
        assert(engine.HasDelta());

        auto* delta = engine.GetDeltaContext();
        assert(delta != nullptr);
        assert(delta->DocumentCount() == 2);
        assert(delta->GetDocPath(deltaDocId) == "delta.txt");

        std::unique_ptr<EvalTree> tree(engine.Compile("banana", "AUTB"));
        std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());
        auto results = exec->Execute(engine.GetReader(tree.get()), 5);
        AssertContains(results, deltaDocId, "default reader includes delta runtime");
        assert(engine.GetDocPath(deltaDocId) == "delta.txt");
    }

    std::remove(INDEX_FILE);
    std::remove(DELTA_FILE);
}

} // namespace IndexAccessTests

// ============================================================
// Test 13: merge base and delta into one index
// ============================================================
namespace IndexAccessTests {

void TestIndexContextMerge()
{
    const char* INDEX_FILE = "test_moonshot_merge_base.bin";
    const char* DELTA_FILE = "test_moonshot_merge_base.delta.bin";
    std::remove(INDEX_FILE);
    std::remove(DELTA_FILE);

    {
        IndexContext base;
        Document doc;
        doc.doc_id = 0;
        doc.path = "base.txt";
        doc.title = "base apple";
        doc.body = "base sharedtoken";
        base.AddDocument(doc);
        assert(base.SaveIndex(INDEX_FILE));
    }

    {
        IndexContext delta("", INDEX_FILE);
        Document doc;
        doc.doc_id = 1;
        doc.path = "delta.txt";
        doc.title = "delta banana";
        doc.body = "delta sharedtoken";
        delta.AddDocument(doc);
        assert(delta.SaveIndex(DELTA_FILE));
    }

    {
        IndexContext merged("", INDEX_FILE);
        assert(merged.Merge(INDEX_FILE));
    }

    {
        IndexContext merged("", INDEX_FILE, false);
        assert(merged.DocumentCount() == 2);
        assert(merged.GetDocPath(0) == "base.txt");
        assert(merged.GetDocPath(1) == "delta.txt");

        std::unique_ptr<IndexSearchExecutor> exec(merged.GetExecutor());
        {
            std::unique_ptr<EvalTree> tree(merged.Compile("apple", "AUTB"));
            auto results = exec->Execute(merged.GetReader(tree.get()), 5);
            AssertContains(results, 0, "merged base doc");
        }
        {
            std::unique_ptr<EvalTree> tree(merged.Compile("banana", "AUTB"));
            auto results = exec->Execute(merged.GetReader(tree.get()), 5);
            AssertContains(results, 1, "merged delta doc");
        }
        {
            std::unique_ptr<EvalTree> tree(merged.Compile("sharedtoken", "AUTB"));
            auto results = exec->Execute(merged.GetReader(tree.get()), 5);
            AssertContains(results, 0, "merged shared base");
            AssertContains(results, 1, "merged shared delta");
        }
    }

    std::remove(INDEX_FILE);
    std::remove(DELTA_FILE);
}

} // namespace IndexAccessTests

namespace IndexAccessTests {

void TestIndexContextMergeContinuationPostings()
{
    const char* INDEX_FILE = "test_moonshot_merge_continuation.bin";
    const char* DELTA_FILE = "test_moonshot_merge_continuation.delta.bin";
    std::remove(INDEX_FILE);
    std::remove(DELTA_FILE);

    constexpr uint32_t BASE_DOCS = 760;
    constexpr uint32_t DELTA_DOCS = 760;
    const auto vector = BuildHashedEmbedding(g_tokenizer.Tokenize("longmerge zzzafter"));

    {
        IndexContext base;
        auto writer = base.GetWriter();
        for (uint32_t docId = 0; docId < BASE_DOCS; ++docId) {
            writer->Write(g_tokenizer.Tokenize("longmerge"), docId, "Title");
            writer->SetDocImportance(docId, 0.1f);
            writer->SetDocVector(docId, vector);
        }
        writer->Write(g_tokenizer.Tokenize("zzzafter"), BASE_DOCS, "Title");
        writer->SetDocImportance(BASE_DOCS, 0.1f);
        writer->SetDocVector(BASE_DOCS, vector);
        assert(base.SaveIndex(INDEX_FILE));
        std::cout << "  base continuation index saved\n";
    }

    {
        IndexContext delta;
        auto writer = delta.GetWriter();
        for (uint32_t docId = 0; docId < DELTA_DOCS; ++docId) {
            const uint64_t finalDocId = BASE_DOCS + 1 + docId;
            writer->Write(g_tokenizer.Tokenize("longmerge"), finalDocId, "Title");
            writer->SetDocImportance(finalDocId, 0.1f);
            writer->SetDocVector(finalDocId, vector);
        }
        assert(delta.SaveIndex(DELTA_FILE));
        std::cout << "  delta continuation index saved\n";
    }

    {
        IndexContext merged("", INDEX_FILE);
        assert(merged.Merge(INDEX_FILE));
        std::cout << "  continuation index merged\n";
    }

    {
        IndexContext merged("", INDEX_FILE, false);
        assert(merged.DocumentCount() == static_cast<uint64_t>(BASE_DOCS + 1 + DELTA_DOCS));
        std::cout << "  continuation index loaded for readback\n";

        auto reader = merged.GetStreamReader("longmergeT");
        uint32_t seen = 0;
        while (!reader->IsEnd()) {
            if (seen < BASE_DOCS)
                assert(reader->GetDocumentID() == seen);
            else
                assert(reader->GetDocumentID() == static_cast<uint64_t>(BASE_DOCS + 1 + (seen - BASE_DOCS)));
            ++seen;
            reader->GoNext();
        }
        assert(seen == BASE_DOCS + DELTA_DOCS);

        auto afterReader = merged.GetStreamReader("zzzafterT");
        assert(!afterReader->IsEnd());
        assert(afterReader->GetDocumentID() == BASE_DOCS);
        afterReader->GoNext();
        assert(afterReader->IsEnd());
        std::cout << "  merged continuation postings traversed: " << seen << "\n";
    }

    std::remove(INDEX_FILE);
    std::remove(DELTA_FILE);
}

} // namespace IndexAccessTests

// ============================================================
// Test 14: bigram indexing and query
// ============================================================
namespace IndexAccessTests {

void TestBigram()
{
    IndexContext   engine;
    SmartTokenizer tok;
    auto           writer = engine.GetWriter();

    /*
    * Doc 0: "good morning vietnam" — bigrams: good·morning, morning·vietnam
    * Doc 1: "bad morning london"   — bigrams: bad·morning,  morning·london
    * Doc 2: "good night vietnam"   — bigrams: good·night,   night·vietnam
    */
    writer->Write(tok.Tokenize("good morning vietnam"), 0, "Title");
    writer->SetDocImportance(0, 0.9f);
    writer->SetDocVector(0, BuildHashedEmbedding(tok.Tokenize("good morning vietnam")));

    writer->Write(tok.Tokenize("bad morning london"),   1, "Title");
    writer->SetDocImportance(1, 0.7f);
    writer->SetDocVector(1, BuildHashedEmbedding(tok.Tokenize("bad morning london")));

    writer->Write(tok.Tokenize("good night vietnam"),   2, "Title");
    writer->SetDocImportance(2, 0.6f);
    writer->SetDocVector(2, BuildHashedEmbedding(tok.Tokenize("good night vietnam")));
    engine.Build();

    auto* store = engine.GetStore();

    /*
    * Verify bigrams were written into the Title stream posting store.
    */
    /* Helper: build bigram posting-store key (word1 \x1F word2 + stream) */
    auto BK = [](const char* a, const char* b, char st) {
        return std::string(a) + BIGRAM_SEP + b + st;
    };
    const auto goodMorning = tok.Tokenize("good morning vietnam");
    const auto badMorning = tok.Tokenize("bad morning london");
    const auto goodNight = tok.Tokenize("good night vietnam");
    assert(goodMorning.size() == 3);
    assert(badMorning.size() == 3);
    assert(goodNight.size() == 3);

    assert(store->GetPostingList(BK(goodMorning[0].c_str(), goodMorning[1].c_str(), 'T')) != nullptr);
    assert(store->GetPostingList(BK(goodMorning[1].c_str(), goodMorning[2].c_str(), 'T')) != nullptr);
    assert(store->GetPostingList(BK(badMorning[0].c_str(), badMorning[1].c_str(), 'T')) != nullptr);
    assert(store->GetPostingList(BK(goodNight[0].c_str(), goodNight[1].c_str(), 'T')) != nullptr);

    std::cout << "  " << goodMorning[0] << "·" << goodMorning[1] << "T    doc_freq="
              << store->GetPostingList(BK(goodMorning[0].c_str(), goodMorning[1].c_str(), 'T'))->doc_freq() << "\n";
    std::cout << "  " << goodMorning[1] << "·" << goodMorning[2] << "T doc_freq="
              << store->GetPostingList(BK(goodMorning[1].c_str(), goodMorning[2].c_str(), 'T'))->doc_freq() << "\n";

    IndexSearchCompiler compiler;
    auto exec = engine.GetExecutor();

    /*
    * "good morning" on Title.
    * Doc 0 matches bigram good_morningT → scored by both OR arms → highest score.
    * Doc 1 has "morning" but not "good" → excluded by AND arm.
    * Doc 2 has "good" but not "morning" → excluded by AND arm.
    */
    {
        auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("good morning", "T"));
        auto results = exec->Execute(engine.GetReader(tree.get()), 5);

        std::cout << "  search('good morning', T):\n";
        for (auto& r : results)
            std::cout << "    doc=" << ReaderDocumentIDValue(r.doc_id) << "  score=" << r.score << "\n";

        AssertContains   (results, 0, "bigram: doc0 matches good morning");
        AssertNotContains(results, 1, "bigram: doc1 has no good");
        AssertNotContains(results, 2, "bigram: doc2 has no morning");
    }

    /*
    * "morning vietnam" on Title.
    * Doc 0 matches bigram morning_vietnamT → highest score.
    */
    {
        auto tree    = std::unique_ptr<EvalTree>(compiler.Compile("morning vietnam", "T"));
        auto results = exec->Execute(engine.GetReader(tree.get()), 5);

        std::cout << "  search('morning vietnam', T):\n";
        for (auto& r : results)
            std::cout << "    doc=" << ReaderDocumentIDValue(r.doc_id) << "  score=" << r.score << "\n";

        AssertContains   (results, 0, "bigram: doc0 matches morning vietnam");
        AssertNotContains(results, 1, "bigram: doc1 has no vietnam");
        AssertNotContains(results, 2, "bigram: doc2 has no morning");
    }

    /*
    * Verify the compiled EvalTree structure for "good morning" on stream T.
    * Expected:
    *   Or( TermNode("good·morningT"),          ← bigram arm
    *       And( TermNode("goodT"),              ← unigram arm
    *            TermNode("morningT") ) )
    */
    {
        auto tree = std::unique_ptr<EvalTree>(compiler.Compile("good morning", "T"));
        assert(tree && !tree->IsEmpty());
        assert(tree->root->GetType() == NodeType::Or);

        auto* orNode = static_cast<OrNode*>(tree->root.get());
        assert(orNode->children.size() == 2);

        assert(orNode->children[0]->GetType() == NodeType::Term);
        auto* bigramNode = static_cast<TermNode*>(orNode->children[0].get());
        assert(bigramNode->stream_key == goodMorning[0] + BIGRAM_SEP + goodMorning[1] + "T");
        std::cout << "  bigram arm key: " << bigramNode->stream_key << "\n";

        assert(orNode->children[1]->GetType() == NodeType::And);
        std::cout << "  unigram arm: AndNode verified\n";
    }

    /*
    * WeakAndBigram compiles to one reader tree:
    *   OR(WeakAnd(unigrams), OR(adjacent bigrams))
    * This keeps the normal Compile -> GetReader -> Execute architecture.
    */
    {
        IndexContext localEngine;
        auto localWriter = localEngine.GetWriter();
        localWriter->Write(tok.Tokenize("alpha beta standalone"), 0, "Title");
        localWriter->Write(tok.Tokenize("alpha beta gamma standalone"), 1, "Title");
        localWriter->Write(tok.Tokenize("gamma delta standalone"), 2, "Title");
        localWriter->Write(tok.Tokenize("delta epsilon standalone"), 3, "Title");
        localWriter->Write(tok.Tokenize("epsilon zeta standalone"), 4, "Title");
        localEngine.Build();

        auto tree = std::unique_ptr<EvalTree>(compiler.Compile(
            "alpha beta gamma delta epsilon zeta", "T", nullptr, QueryCompileMode::WeakAndBigram));
        if (!tree || tree->IsEmpty() || tree->root->GetType() != NodeType::Or)
            throw std::runtime_error("WeakAndBigram must compile to OR(WeakAnd, OR(bigrams))");

        auto* candidateOr = static_cast<OrNode*>(tree->root.get());
        if (candidateOr->children.size() != 2
            || candidateOr->children[0]->GetType() != NodeType::WeakAnd
            || candidateOr->children[1]->GetType() != NodeType::Or)
            throw std::runtime_error("WeakAndBigram candidate set must be OR(WeakAnd, OR(bigrams))");
        auto* bigramOr = static_cast<OrNode*>(candidateOr->children[1].get());
        if (bigramOr->children.size() != 5)
            throw std::runtime_error("WeakAndBigram bigram branch should contain five adjacent bigrams");

        std::unique_ptr<IndexSearchExecutor> localExec(localEngine.GetExecutor());
        auto results = localExec->Execute(localEngine.GetReader(tree.get()), 10);
        AssertContains(results, 0, "weakand bigram: one adjacent bigram enters recall");
        AssertContains(results, 1, "weakand bigram: two adjacent bigrams match");
        AssertContains(results, 2, "weakand bigram: trailing one adjacent bigram enters recall");
        AssertContains(results, 3, "weakand bigram: middle one adjacent bigram enters recall");
        AssertContains(results, 4, "weakand bigram: final one adjacent bigram enters recall");
        std::cout << "  WeakAndBigram tree: OR(WeakAnd, OR(bigrams)) verified\n";
    }
}

} // namespace IndexAccessTests

namespace IndexAccessTests {

void TestContinuationPostings()
{
    IndexContext engine;
    auto writer = engine.GetWriter();

    constexpr uint32_t DOC_COUNT = 5000;
    const auto vector = BuildHashedEmbedding(g_tokenizer.Tokenize("continuationterm"));
    for (uint32_t docId = 0; docId < DOC_COUNT; ++docId) {
        writer->Write(g_tokenizer.Tokenize("continuationterm"), docId, "Title");
        writer->SetDocImportance(docId, 0.1f);
        writer->SetDocVector(docId, vector);
    }
    engine.Build();

    auto reader = engine.GetStreamReader("continuationtermT");
    uint32_t seen = 0;
    uint64_t expectedDoc = 0;
    while (!reader->IsEnd()) {
        assert(reader->GetDocumentID() == expectedDoc);
        ++seen;
        ++expectedDoc;
        reader->GoNext();
    }

    assert(seen == DOC_COUNT && "Continuation postings must not stop at first block");
    std::cout << "  continuation postings traversed: " << seen << "\n";
}

void TestHeadTermMaxKeyBoundary()
{
    IndexContext engine;
    auto writer = engine.GetWriter();
    const std::string maxToken(HEAD_TERM_KEY_MAX - 1, 'a');
    const std::string tooLongToken(HEAD_TERM_KEY_MAX, 'b');

    writer->Write({maxToken}, 0, "Title");
    writer->SetDocImportance(0, 0.1f);
    writer->SetDocVector(0, BuildHashedEmbedding(std::vector<std::string>{maxToken}));
    writer->Write({tooLongToken}, 1, "Title");
    writer->SetDocImportance(1, 0.1f);
    writer->SetDocVector(1, BuildHashedEmbedding(std::vector<std::string>{tooLongToken}));
    engine.Build();

    auto reader = engine.GetStreamReader((maxToken + "T").c_str());
    assert(!reader->IsEnd());
    assert(reader->GetDocumentID() == 0);
    reader->GoNext();
    assert(reader->IsEnd());

    auto tooLongReader = engine.GetStreamReader((tooLongToken + "T").c_str());
    assert(tooLongReader->IsEnd());
    std::cout << "  max head key found; too-long key skipped\n";
}

void TestTermMphfSameBaseCollision()
{
    PostingStore store;
    std::vector<std::string> terms;
    terms.reserve(256);
    terms.push_back("t28");
    terms.push_back("t66");
    for (uint32_t i = 0; terms.size() < 256; ++i) {
        std::string term = "x" + std::to_string(i);
        terms.push_back(term);
    }

    uint64_t docId = 0;
    for (const auto& term : terms)
        store.AddPosting(term, docId++, 1);

    auto built = IndexSerializer::BuildBlocks(store);
    assert(built.BBR_TotalTerms == terms.size());
    assert(built.BBR_TermMphfHeader.TMH_Magic == TERM_MPHF_MAGIC);
    assert(built.BBR_TermMphfHeader.TMH_SlotCount == terms.size());
    assert(!built.BBR_TermMphfDisplacements.empty());
    assert(!built.BBR_TermMphfEntryPages.empty());

    std::vector<uint8_t> used(terms.size(), 0);
    for (const auto& term : terms) {
        const auto& header = built.BBR_TermMphfHeader;
        const uint64_t bucket = TermMphfHash(term.data(), term.size(), header.TMH_BucketSeed) % header.TMH_BucketCount;
        const int32_t displacement = built.BBR_TermMphfDisplacements[static_cast<size_t>(bucket)];
        const uint64_t slot = displacement < 0
            ? static_cast<uint64_t>(-static_cast<int64_t>(displacement) - 1)
            : TermMphfHash(term.data(), term.size(), TermMphfSlotSeed(header.TMH_SlotSeed, static_cast<uint32_t>(displacement))) % header.TMH_SlotCount;
        assert(slot < used.size());
        assert(!used[static_cast<size_t>(slot)]);
        used[static_cast<size_t>(slot)] = 1;

        const uint64_t byteOffset = slot * sizeof(TermMphfEntry);
        const auto* entry = reinterpret_cast<const TermMphfEntry*>(reinterpret_cast<const uint8_t*>(built.BBR_TermMphfEntryPages.data()) + byteOffset);
        uint64_t fingerprint = TermMphfHash(term.data(), term.size(), header.TMH_FingerprintSeed);
        if (fingerprint == 0) fingerprint = 1;
        assert(entry->LTE_Fingerprint == fingerprint);
    }

    std::cout << "  MPHF same-base collision regression passed\n";
}

void TestTokenizerSnowballStemming()
{
    SmartTokenizer tokenizer;
    const auto tokens = tokenizer.Tokenize("suggested suggests suggesting");
    assert(tokens.size() == 3);
    assert(tokens[0] == "suggest");
    assert(tokens[1] == "suggest");
    assert(tokens[2] == "suggest");

    IndexContext engine;
    Document doc;
    doc.doc_id = 0;
    doc.title = "suggested treatment";
    doc.body = "The paper suggested a possible mechanism.";
    engine.AddDocument(doc);
    engine.Build();

    auto reader = engine.GetStreamReader("suggestT");
    assert(!reader->IsEnd());
    assert(reader->GetDocumentID() == 0);
    std::cout << "  Snowball stems suggested/suggests/suggesting to suggest\n";
}

} // namespace IndexAccessTests

std::map<std::string, std::function<void()>> testRegistry = {
    {"TestBuildIndex",       IndexAccessTests::TestBuildIndex},
    {"TestSingleTermSearch", IndexAccessTests::TestSingleTermSearch},
    {"TestAndSearch",        IndexAccessTests::TestAndSearch},
    {"TestOrSearch",         IndexAccessTests::TestOrSearch},
    {"TestWeakAndSearch",    IndexAccessTests::TestWeakAndSearch},
    {"TestNotSearch",        IndexAccessTests::TestNotSearch},
    {"TestFieldConstraint",  IndexAccessTests::TestFieldConstraint},
    {"TestEvalTree",         IndexAccessTests::TestEvalTree},
    {"TestMultiPhase",       IndexAccessTests::TestMultiPhase},
    {"TestDocImportance",    IndexAccessTests::TestDocImportance},
    {"TestEndToEnd",         IndexAccessTests::TestEndToEnd},
    {"TestDiskPersistence",  IndexAccessTests::TestDiskPersistence},
    {"TestDeltaRuntimeHandoff", IndexAccessTests::TestDeltaRuntimeHandoff},
    {"TestIndexContextMerge", IndexAccessTests::TestIndexContextMerge},
    {"TestIndexContextMergeContinuationPostings", IndexAccessTests::TestIndexContextMergeContinuationPostings},
    {"TestBigram",           IndexAccessTests::TestBigram},
    {"TestContinuationPostings", IndexAccessTests::TestContinuationPostings},
    {"TestHeadTermMaxKeyBoundary", IndexAccessTests::TestHeadTermMaxKeyBoundary},
    {"TestTermMphfSameBaseCollision", IndexAccessTests::TestTermMphfSameBaseCollision},
    {"TestTokenizerSnowballStemming", IndexAccessTests::TestTokenizerSnowballStemming},
};
