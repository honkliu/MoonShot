#include "IndexSearchExecutor.h"
#include "IndexReader.h"
#include <memory>
#include <iostream>

void IndexSearchExecutor::Execute()
{
    // Default execute implementation
    std::cout << "IndexSearchExecutor: Default execute method called" << std::endl;
}

void IndexSearchExecutor::Execute(EvalTree *eval_tree)
{
    if (!eval_tree) {
        std::cout << "IndexSearchExecutor: Null evaluation tree" << std::endl;
        return;
    }
    
    std::cout << "IndexSearchExecutor: Executing evaluation tree" << std::endl;
    // TODO: Implement evaluation tree processing logic
}

void IndexSearchExecutor::Execute(std::shared_ptr<IndexReader> index_reader)
{
    if (!index_reader) {
        std::cout << "IndexSearchExecutor: Null index reader" << std::endl;
        return;
    }
    
    std::cout << "IndexSearchExecutor: Executing with index reader" << std::endl;
    
    while (!index_reader->IsEnd()) {
        index_reader->GoNext();
        uint64_t documentId = index_reader->GetDocumentID();
        
        if (documentId != 0) {
            std::cout << "Processing document ID: " << documentId << std::endl;
            // TODO: Add document processing logic
        }
    }
    
    index_reader->Close();
}
