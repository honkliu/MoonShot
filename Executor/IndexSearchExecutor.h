#ifndef INDEXSEARCHEXECUTOR_H__
#define INDEXSEARCHEXECUTOR_H__

#include "EvalExpression.h"
class IndexSearchExecutor
{
    public: 
        void Execute();
        void Execute(EvalTree *eval_tree);

};

#endif
