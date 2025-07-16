#include "IndexContext.h"
#include "EvalExpression.h"
#include "IndexReader.h"
#include "AdvancedIndexReader.h"
#include "AdvancedIndexWriter.h"
#include "IndexSearchExecutor.h"
#include "IndexSearchCompiler.h"
#include "ConfigParameters.h"
#include "Tokenizer.h"
#include "BlockTable.h"

#include <future>
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
        
        auto documentId = index_reader1->GetDocumentID();

        if (documentId != 0) {
            //Find the document. Do something
        }

        index_reader1->Close();
        index_reader2->Close();
        index_reader3->Close();      
    }

    void TestingEndToEnd()
    {
        // IndexContext expects ConfigParameters*, not a string
        auto config = new ConfigParameters();
        auto context = new IndexContext(config);

        // SmartTokenizer expects no arguments
        auto tokenizer = new SmartTokenizer();
        // Use context for writer, not index_context
        auto index_writer = context->GetWriter();
        // Remove GetNewDocumentID (not implemented), use a dummy id
        uint64_t documentId = 1;
        index_writer->Write(tokenizer->Tokenize("Innovative ids in Conf 2021"), documentId, PostingType::Body);
        index_writer->Write(tokenizer->Tokenize("Conf 2021"), documentId, PostingType::Title);
       
        /*
        * For the Embeddings, no need to do it now, 
        * let me implement it later
        * 
        auto index_ebwriter1 = index_context->GetEBWriter("HNSW");
        auto index_ebwriter2 = index_context->GetEBWriter("IVF");

        index_ebwriter1->Write(tokenizer->Tokenize("Innovative ids in Conf 2021"), documentId, PostingType::Anchor);
        index_ebwriter2->Write(tokenizer->Tokenize("Innovative ids in Conf 2021"));
        */
       /*
        auto is_compiler = new IndexSearchCompiler("AUTBV");

        auto eval_tree = is_compiler->Compile("Innovative ids in Conf 2021", "");
        
        auto index_reader = index_context->GetReader(eval_tree);

        while (true) {
            index_reader->GoNext();
            auto documentId = index_reader->GetDocumentID();

            if (documentId == 0)
                break;
        }
    */
    }
    void TestCompositeRead()
    {
        //std::shared_ptr<IndexSearchCompiler> is_compiler(new IndexSearchCompiler());

        auto is_compiler = new IndexSearchCompiler();

        auto eval_tree = is_compiler->Compile("Innovative ids in Conf 2021");
        
        auto index_reader = index_context->GetReader(eval_tree);

        auto executor = index_context->GetExecutor();
        
        executor->Execute(index_reader);
        //auto result = std::async(std::launch::async, [executor] { return executor->Execute(index_reader)});
        //result.wait();
        index_reader->GoNext();

        index_reader->Close();
/*
        boost:asio:thread_pool thread_pool(4);
        //boost::asio::post(thread_pool, boost::bind(&IndexSearchExecutor::Execute,  ))
        boost::asio::post(thread_pool, [] {
            executor->Execute(index_reader);
        });


        auto a1 = std::async(static_cast<void (*)(Reader)>(&IndexSearchExecutor::Executor), executor, index_reader);
        a1.wait();
        a1.get();

         or
         auto function = static_cast<void(*)(Reader)>(Exextue);
         audo call = std::async(funciton, reader, ...)
         call.wait

         or
         auto a = std::async([](Reader reader) {
                                    exector->execute(reader);
                                    },
                                reader
         )

         std::async(std::launch::async, [executor] { return executor->Execute(index_reader)})

         or use:

class A
{
public:
    int foo(int a, int b);
    int foo(int a, double b);
};
         std::function<int(int,double)> func = std::bind((int(A::*)(int,double))&A::foo,&a,std::placeholders::_1,std::placeholders::_2);
auto f = std::async(std::launch::async, func, 2, 3.5);
    
    std::bind(
            (
            int(A::*)(int,double)
            )&A::foo,
            &a,
            std::placeholders::_1,
            std::placeholders::_2);
    }
            */
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
        auto index_reader = index_context->GetReader(embedding);

        index_reader->GoNext();
    }
}