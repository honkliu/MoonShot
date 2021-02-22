class ShenNongImp final: public ShenNong::Service {

    Status Query121(ServerContext * context, const Question * question, Answers *answers) override 
    {
        return Status::OK;
    }

    Status Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer) override 
    {
        return Status::OK;
    }

    
    Status Query129(ServerContext * context, ServerReader<Question>* reader, Answers* answers) override 
    {

        while (reader->Read() {
            writer->Write()
        })
        return Status::OK;
    }

    Status Query929(ServerContext* context, ServerReader<Question>* reader, ServerWriter<Answers>* writer) override 
    {

        return Status::OK;

    }       
};