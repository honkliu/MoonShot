
Status ShenNongImp::Query121(ServerContext * context, const Question * question, Answers *answers) override 
{
    return Status::OK;
}

Status ShenNongImp::Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer) override 
{
    return Status::OK;
}


Status ShenNongImp::Query129(ServerContext * context, ServerReader<Question>* reader, Answers* answers) override 
{

/*
    while (reader->Read() {
        writer->Write()
    })
*/
    return Status::OK;
}

Status ShenNongImp::Query929(ServerContext* context, ServerReader<Question>* reader, ServerWriter<Answers>* writer) override 
{

    return Status::OK;

}
