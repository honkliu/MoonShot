class ShenNongImp final: public ShenNong::Service {

    Status Query121(ServerContext * context, const Question * question, Answers *answers);

    Status Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer);
    
    Status Query129(ServerContext * context, ServerReader<Question>* reader, Answers* answers);

    Status Query929(ServerContext* context, ServerReader<Question>* reader, ServerWriter<Answers>* writer);
};