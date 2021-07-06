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
        auto index_reader1 = index_context->GetReader("Innovative");
        auto index_reader2 = index_context->GetReader("Ideas");
        auto index_reader3 = index_context->GetReader("Conf2021");

        index_reader1->GoNext();
        index_reader2->GoNext();
        index_reader3->GoNext();

        index_reader1->Close();
        index_reader2->Close();
        index_reader3->Close();      
    }

    void TestCompositeRead()
    {
        //std::shared_ptr<IndexSearchCompiler> is_compiler(new IndexSearchCompiler());

        auto is_compiler = new IndexSearchCompiler();

        auto eval_tree = is_compiler->Compile("Innovative ids in Conf 2021");
        
        auto index_reader = index_context->GetReader(eval_tree);

        auto executor = index_context->GetExecutor();
        executor->Execute(eval_tree);

        index_reader->GoNext();

        index_reader->Close();
    }

    void TestVectorRead()
    {
        auto is_compiler = new IndexSearchCompiler();

        /*
        * The call here must specify the return type
        */
        auto embedding = is_compiler->CompileToVector<float>("Innovitve ideas in Conf 2021");

        /*
        * or use index_context->GetReader<float>(embedding);
        */
        auto index_reader = index_context->GetReader(embedding)

        index_reader->GoNext();
    }
}

 