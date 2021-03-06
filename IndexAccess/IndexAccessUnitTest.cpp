#include "IndexContext.h"
#include "EvalExpression.h"
#include "IndexReader.h"
#include "AdvancedIndexReader.h"
#include "IndexSearchExecutor.h"
#include "IndexSearchCompiler.h"
#include "ConfigParameters.h"

namespace IndexAccessTests
{
    IndexContext* index_context = nullptr;

    void SetupIndex(const char * filename, IndexBlockTable * table, ConfigParameters * config_paramter)
    {
        index_context = new IndexContext(config_paramter);
        index_context->SetupContext(filename, table);
    }

    void TestSingleRead()
    {        
        IndexReader * index_reader1 = index_context->GetReader("Innovative");
        IndexReader * index_reader2 = index_context->GetReader("Ideas");
        IndexReader * index_reader3 = index_context->GetReader("Conf2021");

        index_reader1->GoNext();
        index_reader1->Close();
        index_reader2->Close();
        index_reader3->Close();      
    }

    void TestCompositeRead()
    {
        //boost::shared_ptr<IndexSearchCompiler> is_compiler(new IndexSearchCompiler());

        auto is_compiler = new IndexSearchCompiler();

        EvalTree * eval_tree = is_compiler->Compile("Innative ids in Conf 2021");
        
        IndexReader * index_reader = index_context->GetReader(eval_tree);

        IndexSearchExecutor * executor = index_context->GetExecutor();
        executor->Execute(eval_tree);

        index_reader->GoNext();

        index_reader->Close();
    }
}

 