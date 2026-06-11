/*
* demo.cpp — end-to-end MoonShot example.
*
* BuildIndex : tokenise five documents and save the index to disk.
* SearchIndex: reload from disk, compile queries, rank with BM25.
*/

#include "IndexContext.h"

#include <cstdio>
#include <cstring>

static const char* INDEX_FILE = "moonshot_demo.idx";

/* ------------------------------------------------------------------ */

struct Doc {
    uint64_t    id;
    const char* anchor;
    const char* url;
    const char* title;
    const char* body;
    float       importance; /* PageRank-style signal, boosts BM25 score */
};

static const Doc kDocs[] = {
    {
        1,
        "Honda Race Car Toy",
        "www.honda.com/toys/race-car",
        "Honda Miniature Race Car Toy",
        "Honda's official die-cast miniature race car toy for collectors. "
        "Precision-engineered scale model of the Honda RC213V MotoGP race car. "
        "Suitable for racing enthusiasts and children aged 8 and up.",
        1.2f
    },
    {
        2,
        "Honda Civic Sport",
        "www.honda.com/civic/sport",
        "Honda Civic Sport 2024",
        "The Honda Civic Sport delivers turbocharged performance with a sleek "
        "design. Fuel-efficient engine, adaptive suspension, and Apple CarPlay. "
        "The ideal everyday sports car for modern drivers.",
        1.0f
    },
    {
        3,
        "Toyota Race Car Collection",
        "www.toyota.com/motorsport/collection",
        "Toyota Motorsport Race Car Models",
        "Official Toyota GR motorsport race car scale models and toy collection. "
        "Includes the Toyota GR010 Le Mans hypercar and GR Yaris rally car toys. "
        "Great gifts for racing fans of all ages.",
        0.9f
    },
    {
        4,
        "Best Toy Cars 2024",
        "www.google.com/shopping/toy-cars",
        "Top Toy Race Cars 2024 Google Shopping",
        "Google Shopping curated list of the best toy race cars available today. "
        "Includes die-cast models from Honda, Toyota, Ferrari and Hot Wheels. "
        "Compare prices and read reviews before you buy.",
        0.7f
    },
    {
        5,
        "Formula One Engineering Guide",
        "www.motorsport.org/f1-guide",
        "Formula One Race Car Aerodynamics",
        "A technical guide to Formula One race car design and aerodynamics. "
        "Covers wing configuration, downforce, tire compounds, and pit strategy. "
        "Not affiliated with Honda, Toyota or any other manufacturer.",
        0.6f
    },
};

/* ------------------------------------------------------------------ */

void BuildIndex()
{
    puts("\n=== BuildIndex ===");

    /*
    * Construct with no index path so nothing is loaded from disk.
    * GetWriter() returns an AdvancedIndexWriter backed by a PostingStore.
    */
    IndexContext ctx;
    auto writer = ctx.GetWriter();
    auto tok    = ctx.GetTokenizer();

    for (const auto& doc : kDocs) {
        writer->Write(tok->Tokenize(doc.anchor), doc.id, "Anchor");
        writer->Write(tok->Tokenize(doc.url),    doc.id, "URL");
        writer->Write(tok->Tokenize(doc.title),  doc.id, "Title");
        writer->Write(tok->Tokenize(doc.body),   doc.id, "Body");
        writer->SetDocImportance(doc.id, doc.importance);

        printf("  indexed doc %llu: %s\n",
               (unsigned long long)doc.id, doc.title);
    }

    ctx.SaveIndex(INDEX_FILE);
    printf("  saved → %s\n", INDEX_FILE);
}

/* ------------------------------------------------------------------ */

void SearchIndex()
{
    puts("\n=== SearchIndex ===");

    IndexContext ctx("", INDEX_FILE);
    auto compiler = ctx.GetCompiler();
    auto executor = ctx.GetExecutor();

    auto run = [&](const char* q) {
        auto tree   = std::unique_ptr<EvalTree>(compiler->Compile(q));
        auto reader = ctx.GetReader(tree.get());
        auto hits   = executor->Execute(reader);

        printf("\n[%s]\n", q);
        for (const auto& h : hits)
            printf("  doc %llu  score=%.3f\n",
                   (unsigned long long)h.doc_id, h.score);
    };

    run("honda");
    run("race car");
    run("race car honda");
    run("toy race car");
    run("honda toyota");

    /* debug trace — readers print each doc they visit */
    puts("\n--- trace: race car NOT toy ---");
    {
        auto tree   = std::unique_ptr<EvalTree>(compiler->Compile("race car toy"));
        auto reader = ctx.GetReader(tree.get());
        reader->SetDebug("race car toy");
        auto hits = executor->Execute(reader);

        puts("\n  results:");
        for (const auto& h : hits)
            printf("    doc %llu  score=%.3f\n",
                   (unsigned long long)h.doc_id, h.score);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char* argv[])
{
    bool doBuild  = false;
    bool doSearch = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-b") == 0) doBuild  = true;
        if (strcmp(argv[i], "-q") == 0) doSearch = true;
    }

    if (!doBuild && !doSearch) {
        puts("usage: demo -b       build index\n"
             "            -q       search index\n"
             "            -b -q    build then search");
        return 1;
    }

    if (doBuild)  BuildIndex();
    if (doSearch) SearchIndex();
    return 0;
}
