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

using Wenda::ShenNong;
using Wenda::Question;
using Wenda::Answers;

class ShenNongImp final: public ShenNong::Service {
    Status Query121(ServerContext * context, const Question * question, Answers *answers);

    Status Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer);
    
    Status Query921(ServerContext * context, ServerReader<Question>* reader, Answers* answers);

    Status Query929(ServerContext* context, ServerReader<Question>* reader, ServerWriter<Answers>* writer);
    
};
#endif