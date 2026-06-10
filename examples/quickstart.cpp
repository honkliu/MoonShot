/*
 * quickstart.cpp — minimal MoonShot usage: index three documents, search, save, reload.
 *
 * Build with the SDK (see docs/manual.md §3 for CMake integration):
 *
 *   cmake -B build && cmake --build build --target quickstart
 *   ./build/examples/quickstart
 */

#include "moonshot.h"
#include <cstdio>

int main()
{
    /* ------------------------------------------------------------------ *
     * 1.  Create the search engine.                                        *
     *     Pass a file path so the index can be saved and reloaded.         *
     * ------------------------------------------------------------------ */
    IndexContext engine("", "my_index.bin");
    SmartTokenizer tok;

    /* ------------------------------------------------------------------ *
     * 2.  Index documents.                                                 *
     *     Write() accepts tokens, a document id, and a stream name.        *
     * ------------------------------------------------------------------ */
    auto writer = engine.GetWriter();

    writer->Write(tok.Tokenize("Fast and safe systems programming"),   1, "Body");
    writer->Write(tok.Tokenize("Rust programming language"),           1, "Title");
    writer->SetDocImportance(1, 0.9f);

    writer->Write(tok.Tokenize("Machine learning with Python"),        2, "Body");
    writer->Write(tok.Tokenize("Python data science"),                 2, "Title");
    writer->SetDocImportance(2, 0.7f);

    writer->Write(tok.Tokenize("Concurrent programs with goroutines"), 3, "Body");
    writer->Write(tok.Tokenize("Go language concurrency"),             3, "Title");
    writer->SetDocImportance(3, 0.6f);

    printf("Indexed %llu documents.\n\n", engine.GetStore()->TotalDocs());

    /* ------------------------------------------------------------------ *
     * 3.  Search.                                                          *
     * ------------------------------------------------------------------ */
    IndexSearchCompiler compiler;

    auto runQuery = [&](const char* q, const char* streams) {
        printf("search(\"%s\", \"%s\"):\n", q, streams);
        std::unique_ptr<EvalTree>            tree(compiler.Compile(q, streams));
        std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());
        for (auto& r : exec->Execute(engine.GetReader(tree.get()), 5))
            printf("  doc=%-3llu  score=%.3f\n", r.doc_id, r.score);
        printf("\n");
    };

    runQuery("rust",              "AUTB");
    runQuery("programming",       "AUTB");
    runQuery("python OR go",      "AUTB");
    runQuery("title:concurrency", "T");

    /* ------------------------------------------------------------------ *
     * 4.  Persist the index to disk, then reload and search again.         *
     * ------------------------------------------------------------------ */
    if (engine.SaveIndex())
        printf("Index saved to my_index.bin\n\n");

    IndexContext engine2("", "my_index.bin");   // auto-loads from disk
    printf("Reloaded %llu documents from disk.\n\n",
           engine2.GetStore()->TotalDocs());

    std::unique_ptr<EvalTree>            tree(compiler.Compile("rust", "AUTB"));
    std::unique_ptr<IndexSearchExecutor> exec(engine2.GetExecutor());
    printf("search(\"rust\") after reload:\n");
    for (auto& r : exec->Execute(engine2.GetReader(tree.get()), 5))
        printf("  doc=%-3llu  score=%.3f\n", r.doc_id, r.score);

    return 0;
}
