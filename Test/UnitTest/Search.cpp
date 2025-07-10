#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <functional>

// ===================
// Tokenization System
// ===================
class Tokenizer {
public:
    Tokenizer(const std::string& config) : config(config) {
        if (config.find("Unigram") != std::string::npos) unigram = true;
        if (config.find("Bigram") != std::string::npos) bigram = true;
    }

    std::vector<std::string> Tokenize(const std::string& text) {
        std::vector<std::string> tokens;
        
        // Unigram tokenization (split by spaces)
        if (unigram) {
            size_t start = 0, end = 0;
            while ((end = text.find(' ', start)) != std::string::npos) {
                if (end != start) {
                    tokens.push_back(text.substr(start, end - start));
                }
                start = end + 1;
            }
            if (start < text.length()) {
                tokens.push_back(text.substr(start));
            }
        }
        
        // Bigram tokenization
        if (bigram && text.length() >= 2) {
            for (size_t i = 0; i <= text.length() - 2; i++) {
                tokens.push_back(text.substr(i, 2));
            }
        }
        
        return tokens;
    }

private:
    std::string config;
    bool unigram = false;
    bool bigram = false;
};

// =====================
// Index Context Manager
// =====================
class IndexContext {
public:
    explicit IndexContext(const std::string& tenant) : tenant(tenant) {}
    
    const std::string& GetTenant() const { return tenant; }

private:
    std::string tenant;
};

// ==================
// Writer Base Class
// ==================
class IndexWriter {
public:
    virtual void Write(const std::vector<std::string>& tokens) = 0;
    virtual void Write(const std::string& text) = 0;
    virtual ~IndexWriter() = default;
};

// =====================
// Embedding-Based Writer
// =====================
class EBWriter {
public:
    explicit EBWriter(const std::string& algorithm) : algorithm(algorithm) {}
    
    void Write(const std::vector<std::string>& tokens) {
        std::cout << "EBWriter [" << algorithm << "] indexing " 
                  << tokens.size() << " tokens\n";
        // Actual embedding indexing would happen here
    }

private:
    std::string algorithm;
};

// =====================
// Search Query Compiler
// =====================
class IndexSearchCompiler {
public:
    explicit IndexSearchCompiler(const std::string& config) : config(config) {}
    
    class EvalTree {
    public:
        void Execute() {
            std::cout << "Executing query evaluation tree\n";
        }
    };
    
    EvalTree* Compile(const std::string& query, const std::string& options) {
        std::cout << "Compiling query: " << query << " with options: " 
                  << options << "\n";
        return new EvalTree();
    }

private:
    std::string config;
};

// ==================
// Index Reader
// ==================
class IndexReader {
public:
    IndexReader(int max_docs) : max_docs(max_docs), current_id(0) {}
    
    void GoNext() {
        current_id++;
    }
    
    int GetDocumentID() {
        return (current_id <= max_docs) ? current_id : 0;
    }

private:
    int max_docs;
    int current_id;
};

// =====================
// Enhanced IndexContext
// =====================
class EnhancedIndexContext : public IndexContext {
public:
    using IndexWriterFactory = std::function<IndexWriter*(Tokenizer*)>;
    
    EnhancedIndexContext(const std::string& tenant) : IndexContext(tenant) {
        // Register default writer types
        RegisterWriterType("A", [](Tokenizer* t) { return new DefaultIndexWriter(); });
        RegisterWriterType("U", [](Tokenizer* t) { return new DefaultIndexWriter(); });
        RegisterWriterType("B", [](Tokenizer* t) { return new TokenizingIndexWriter(t); });
    }
    
    void RegisterWriterType(const std::string& name, IndexWriterFactory factory) {
        writer_factories[name] = factory;
    }
    
    IndexWriter* GetWriter(const std::string& name, Tokenizer* tokenizer = nullptr) {
        if (writer_factories.find(name) != writer_factories.end()) {
            return writer_factories[name](tokenizer);
        }
        return new DefaultIndexWriter();
    }
    
    EBWriter* GetEBWriter(const std::string& algorithm) {
        return new EBWriter(algorithm);
    }
    
    IndexReader* GetReader(IndexSearchCompiler::EvalTree* eval_tree) {
        eval_tree->Execute();
        return new IndexReader(5);  // Return reader with 5 mock docs
    }

private:
    // =====================
    // Concrete Writer Implementations
    // =====================
    class DefaultIndexWriter : public IndexWriter {
    public:
        void Write(const std::vector<std::string>& tokens) override {
            std::cout << "Indexing " << tokens.size() << " tokens\n";
            // Actual token indexing would happen here
        }
        
        void Write(const std::string& text) override {
            std::cout << "Received raw text, but no tokenizer provided!\n";
        }
    };
    
    class TokenizingIndexWriter : public IndexWriter {
    public:
        explicit TokenizingIndexWriter(Tokenizer* tokenizer) : tokenizer(tokenizer) {}
        
        void Write(const std::vector<std::string>& tokens) override {
            std::cout << "TokenizingIndexWriter: Direct token input - " 
                      << tokens.size() << " tokens\n";
        }
        
        void Write(const std::string& text) override {
            if (tokenizer) {
                auto tokens = tokenizer->Tokenize(text);
                std::cout << "TokenizingIndexWriter: Tokenized text to " 
                          << tokens.size() << " tokens\n";
            } else {
                std::cout << "TokenizingIndexWriter: No tokenizer available!\n";
            }
        }
        
    private:
        Tokenizer* tokenizer;
    };
    
    std::map<std::string, IndexWriterFactory> writer_factories;
};

// =====================
// End-to-End Test
// =====================
int main() {
    // 1. Create index context
    auto context = new EnhancedIndexContext("Tenant ABC");
    
    // 2. Create tokenizer
    auto tokenizer = new Tokenizer("Unigram, Bigram");
    
    // 3. Get different types of index writers
    auto index_writer1 = context->GetWriter("A");
    auto index_writer2 = context->GetWriter("U");
    auto index_writer3 = context->GetWriter("B", tokenizer);
    
    // 4. Write content using various methods
    auto tokens = tokenizer->Tokenize("Innovative ids in Conf 2021");

    std::cout << "Tokens (" << tokens.size() << "):\n";
    for (const auto& token : tokens) {
    	std::cout << " - '" << token << "'\n";
    }
    std::cout << std::flush;  // Ensure immediate output
			  //
    index_writer1->Write(tokens);
    index_writer2->Write(tokens);
    index_writer3->Write("Innovative ids in Conf 2021");
    
    // 5. Get embedding-based writers
    auto index_ebwriter1 = context->GetEBWriter("HNSW");
    auto index_ebwriter2 = context->GetEBWriter("IVF");
    
    // 6. Write to embedding indexes
    index_ebwriter1->Write(tokens);
    index_ebwriter2->Write(tokens);
    
    // 7. Compile search query
    auto is_compiler = new IndexSearchCompiler("AUTBV");
    auto eval_tree = is_compiler->Compile("Innovative ids in Conf 2021", "");
    
    // 8. Execute search and iterate through results
    auto index_reader = context->GetReader(eval_tree);
    while (true) {
        index_reader->GoNext();
        auto documentId = index_reader->GetDocumentID();
        if (documentId == 0) break;
        std::cout << "Retrieved document ID: " << documentId << "\n";
    }
    
    // Cleanup
    delete index_reader;
    delete eval_tree;
    delete is_compiler;
    delete index_ebwriter2;
    delete index_ebwriter1;
    delete index_writer3;
    delete index_writer2;
    delete index_writer1;
    delete tokenizer;
    delete context;
    
    return 0;
}
//g++ Search.cpp -o SearchTest
