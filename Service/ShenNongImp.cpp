#include <grpcpp/grpcpp.h>

#include "ShenNongImp.h"

using grpc::ServerContext;
using grpc::Status;
using Wenda::ShenNong;
using Wenda::Question;
using Wenda::Answers;

Status ShenNongImp::Query121(ServerContext * context, const Question * question, Answers *answers)
{
    return Status::OK;
}


Status ShenNongImp::Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer)  
{
    return Status::OK;
}


Status ShenNongImp::Query921(ServerContext * context, ServerReader<Question>* reader, Answers* answers)  
{

/*
    while (reader->Read() {
        writer->Write()
    })
*/
    return Status::OK;
}

Status ShenNongImp::Query929(ServerContext* context, ServerReaderWriter<Answers, Question>* readerwriter)   
{

    return Status::OK;

}
