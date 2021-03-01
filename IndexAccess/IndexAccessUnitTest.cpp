namespace IndexAccessTests
{
    IndexContext* index_context = nullptr

    void SetupIndex(const char * filename, IndexBlockTable * table = nullptr, ConfigParamters * config_paramter)
    {
        index_context = new IndexContext(config_paramter);
        index_context->SetupContext(filename, table);
    }

    void TestSingleRead()
    {        
        IndexReader * index_reader1 = index_context->GetCandidatesByToken("Innovative");
        IndexReader * index_reader2 = index_context->GetCandidatesByToken("Ideas");
        IndexReader * index_reader3 = index_context->GetCandidatesByToken("Conf2021");

        index_reader1->close();
        index_reader2->close();
        index_reader3->close();      
    }

    void TestCompositeRead()
    {
        boost::shared_ptr<IndexSearchCompiler> is_compiler = new IndexSearchCompiler;

        ExecutionMap map = is_compiler.compile("Innative ids in Conf 2021");
        
        IndexReader * index_reader1 = index_context->GetCandidatesByToken("Innovative");
        IndexReader * index_reader2 = index_context->GetCandidatesByToken("Ideas");
        IndexReader * index_reader3 = index_context->GetCandidatesByToken("Conf2021");

        index_reader1->close();
        index_reader2->close();
        index_reader3->close();      
    }
}

 