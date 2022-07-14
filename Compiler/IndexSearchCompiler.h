#ifndef INDEXSEARCHCOMPILER_H__
#define INDEXSEARCHCOMPILER_H__

#include <string>

#include "EvalExpression.h"
#include "Embeddings.h"

class IndexSearchCompiler
{
    public:
        IndexSearchCompiler(){}
        EvalTree * Compile(const char * query_string)
        {
            return NULL; 
        }
        EvalTree * Compile(const std::string& query_string)
        {
            return NULL; 
        }

        template<typename T>
        Embeddings<T> * CompileToVector(const char * query_string)
        {
            return NULL;
        }
};
#endif