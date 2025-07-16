#ifndef INDEXSEARCHEXECUTOR_H__
#define INDEXSEARCHEXECUTOR_H__

#include "EvalExpression.h"
#include "IndexReader.h"
#include <memory>

class IndexSearchExecutor
{
    public: 
        void Execute();
        void Execute(EvalTree *eval_tree);
        void Execute(std::shared_ptr<IndexReader> index_reader);
};

#endif
