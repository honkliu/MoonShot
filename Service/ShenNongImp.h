#ifndef SHENNONGIMP_H_
#define SHENNONGIMP_H_

#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "MatchService.grpc.pb.h"
#include "MatchService.pb.h"

using grpc::ServerContext;
using grpc::Status;
using grpc::ServerWriter;
using grpc::ServerReader;
using grpc::ServerReaderWriter;

using Wenda::ShenNong;
using Wenda::Question;
using Wenda::Answers;

class ShenNongImp final: public ShenNong::Service {
    Status Query121(ServerContext * context, const Question * question, Answers *answers) override;

    Status Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer) override;
    
    Status Query921(ServerContext * context, ServerReader<Question>* reader, Answers* answers) override;

    Status Query929(ServerContext* context, ServerReaderWriter<Answers, Question>* readerwriter) override;
    
};
#endif